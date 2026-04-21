#include "fs/pipe.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "sched/process.h"

/* how many are alive. only for saying so -- nothing depends on it */
static size_t live;

void pipe_reset(struct pipe *p) {
    memset(p, 0, sizeof *p);
    spin_init(&p->lock, "pipe", LOCK_RANK_PIPE);
    p->readers = 1;
    p->writers = 1;
}

/* ---- the ring ------------------------------------------------------
 *
 * head is where the next byte comes out; count says how many are in
 * there. keeping a count rather than a second index is what makes full
 * and empty tell themselves apart -- with two indices they look
 * identical and every ring buffer bug in history lives in that gap. */

uint32_t pipe_pending(const struct pipe *p) { return p->count; }
uint32_t pipe_room(const struct pipe *p)    { return PIPE_BUF - p->count; }

uint32_t pipe_put(struct pipe *p, const void *buf, uint32_t len) {
    uint32_t room = pipe_room(p);
    if (len > room) {
        len = room;
    }

    const uint8_t *src = buf;
    uint32_t tail = (p->head + p->count) % PIPE_BUF;

    /* up to the end of the buffer, then round to the front. two copies
     * rather than a loop, because the wrap is the only interesting
     * moment and it should be visible */
    uint32_t first = PIPE_BUF - tail;
    if (first > len) {
        first = len;
    }
    memcpy(&p->data[tail], src, first);
    memcpy(&p->data[0], src + first, len - first);

    p->count += len;
    return len;
}

uint32_t pipe_get(struct pipe *p, void *buf, uint32_t len) {
    if (len > p->count) {
        len = p->count;
    }

    uint8_t *dst = buf;
    uint32_t first = PIPE_BUF - p->head;
    if (first > len) {
        first = len;
    }
    memcpy(dst, &p->data[p->head], first);
    memcpy(dst + first, &p->data[0], len - first);

    p->head = (p->head + len) % PIPE_BUF;
    p->count -= len;
    return len;
}

/* ---- lifetime ------------------------------------------------------ */

struct pipe *pipe_create(void) {
    struct pipe *p = kmalloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    pipe_reset(p);
    live++;
    return p;
}

/* both ends gone means nobody can ever refer to it again, so it goes.
 * called with the lock dropped, since freeing it while holding a lock
 * that lives inside it is a way to be reading freed memory */
static void maybe_free(struct pipe *p) {
    uint64_t flags = spin_lock_irq(&p->lock);
    bool dead = (p->readers == 0 && p->writers == 0);
    spin_unlock_irq(&p->lock, flags);

    if (dead) {
        live--;
        kfree(p);
    }
}

void pipe_close_read(struct pipe *p) {
    if (p == NULL) {
        return;
    }
    uint64_t flags = spin_lock_irq(&p->lock);
    if (p->readers > 0) {
        p->readers--;
    }
    /* a writer parked waiting for room has to find out that the room is
     * never coming. it wakes, sees no readers, and fails */
    waitq_wake_all(&p->writable);
    spin_unlock_irq(&p->lock, flags);

    maybe_free(p);
}

void pipe_close_write(struct pipe *p) {
    if (p == NULL) {
        return;
    }
    uint64_t flags = spin_lock_irq(&p->lock);
    if (p->writers > 0) {
        p->writers--;
    }
    /* this is end of file arriving. a reader asleep on an empty pipe
     * wakes, finds nothing and no writers, and returns 0 -- which is
     * the entire mechanism by which a pipeline ever finishes */
    waitq_wake_all(&p->readable);
    spin_unlock_irq(&p->lock, flags);

    maybe_free(p);
}

void pipe_release_for(int pid) {
    if (pid == 0) {
        return;
    }
    /* taken off the process first and closed after: closing one wakes
     * threads, and the process table's lock may not be held while
     * reaching up to the scheduler */
    struct pipe *in, *out;
    process_take_pipes(pid, &in, &out);
    pipe_close_read(in);
    pipe_close_write(out);
}

size_t pipe_count(void) { return live; }

/* ---- reading and writing, which is where the blocking is ----------- */

int64_t pipe_read(struct pipe *p, int pid, void *buf, uint64_t len) {
    if (p == NULL || len == 0) {
        return -1;
    }
    if (len > PIPE_BUF) {
        len = PIPE_BUF;
    }

    uint64_t flags = spin_lock_irq(&p->lock);

    while (p->count == 0) {
        if (p->writers == 0) {
            spin_unlock_irq(&p->lock, flags);
            return 0;           /* end of file, and the only kind there is */
        }
        if (process_interrupt_pending(pid)) {
            spin_unlock_irq(&p->lock, flags);
            return -1;
        }

        /* on the queue first, then let go, then sleep. a wake landing
         * in that gap has already marked me ready, so the sleep returns
         * at once rather than being missed -- and the lock is never
         * held across the switch, because a sleeping thread holding one
         * is a machine where nobody else can ever have it */
        waitq_enqueue(&p->readable);
        spin_unlock_irq(&p->lock, flags);
        waitq_sleep();
        flags = spin_lock_irq(&p->lock);
    }

    uint32_t n = pipe_get(p, buf, (uint32_t)len);

    /* somebody may be waiting for the room I just made */
    waitq_wake_all(&p->writable);
    spin_unlock_irq(&p->lock, flags);
    return (int64_t)n;
}

int64_t pipe_write(struct pipe *p, int pid, const void *buf, uint64_t len) {
    if (p == NULL || len == 0) {
        return (p == NULL) ? -1 : 0;
    }

    const uint8_t *src = buf;
    uint64_t done = 0;

    uint64_t flags = spin_lock_irq(&p->lock);

    while (done < len) {
        if (p->readers == 0) {
            spin_unlock_irq(&p->lock, flags);
            /* what was already written stays written. a partial write
             * followed by a broken pipe is what really happened, and
             * saying so is more use than pretending none of it went */
            return (done > 0) ? (int64_t)done : -1;
        }
        if (p->count == PIPE_BUF) {
            if (process_interrupt_pending(pid)) {
                spin_unlock_irq(&p->lock, flags);
                return (done > 0) ? (int64_t)done : -1;
            }
            waitq_enqueue(&p->writable);
            spin_unlock_irq(&p->lock, flags);
            waitq_sleep();
            flags = spin_lock_irq(&p->lock);
            continue;
        }

        done += pipe_put(p, src + done, (uint32_t)(len - done));

        /* wake the reader for each chunk rather than at the end. a
         * pipeline where the writer never stops would otherwise never
         * let the reader see anything, which is not a pipeline, it is a
         * file being written slowly */
        waitq_wake_all(&p->readable);
    }

    spin_unlock_irq(&p->lock, flags);
    return (int64_t)done;
}
