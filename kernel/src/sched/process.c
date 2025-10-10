#include "sched/process.h"
#include "cpu/interrupts.h"
#include "lib/string.h"

/* a fixed table rather than a linked list of allocations. thirty-two is
 * more programs than this machine will ever usefully run at once, and a
 * fixed table has no lifetime questions at all -- which matters a great
 * deal for the one structure whose job is to outlive things */
static struct process table[MAX_PROCESSES];
static int next_pid = 1;

static struct process *slot_for(int pid) {
    if (pid <= 0) {
        return NULL;
    }
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid == pid) {
            return &table[i];
        }
    }
    return NULL;
}

int process_create(const char *name, int parent, uint64_t now_ms) {
    uint64_t flags = irq_save();
    int pid = 0;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0) {
            continue;
        }
        struct process *p = &table[i];

        p->pid        = next_pid++;
        p->parent     = parent;
        p->thread_id  = 0;
        p->exited     = false;
        p->exit_code  = 0;
        p->started_ms = now_ms;
        p->ended_ms   = 0;
        for (size_t f = 0; f < MAX_FDS; f++) {
            p->fds[f].open = false;
        }

        size_t n = 0;
        while (name[n] != '\0' && n < PROC_NAME_MAX - 1) {
            p->name[n] = name[n];
            n++;
        }
        p->name[n] = '\0';

        pid = p->pid;
        break;
    }

    irq_restore(flags);
    return pid;
}

void process_set_thread(int pid, int thread_id) {
    uint64_t flags = irq_save();
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->thread_id = thread_id;
    }
    irq_restore(flags);
}

void process_exited(int pid, int code, uint64_t now_ms) {
    uint64_t flags = irq_save();
    struct process *p = slot_for(pid);
    /* the first answer is the true one: a process killed while it was
     * already on its way out should not have its code overwritten */
    if (p != NULL && !p->exited) {
        p->exited    = true;
        p->exit_code = code;
        p->ended_ms  = now_ms;
        p->thread_id = 0;
        /* whatever it had open closes with it. nothing was allocated,
         * so this is only bookkeeping -- but leaving them open would
         * make the numbers in `ps` a lie */
        for (size_t f = 0; f < MAX_FDS; f++) {
            p->fds[f].open = false;
        }
    }
    irq_restore(flags);
}

bool process_collect(int pid, int *code) {
    uint64_t flags = irq_save();
    bool collected = false;

    struct process *p = slot_for(pid);
    if (p != NULL && p->exited) {
        if (code != NULL) {
            *code = p->exit_code;
        }
        p->pid = 0;         /* the slot is free again */
        collected = true;
    }

    irq_restore(flags);
    return collected;
}

const struct process *process_find(int pid) {
    return slot_for(pid);
}

const struct process *process_at(size_t index) {
    size_t seen = 0;
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid == 0) {
            continue;
        }
        if (seen == index) {
            return &table[i];
        }
        seen++;
    }
    return NULL;
}

size_t process_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0) {
            n++;
        }
    }
    return n;
}

/* ---- open files ---------------------------------------------------- */

int process_fd_open(int pid, const void *data, uint64_t size) {
    uint64_t flags = irq_save();
    int fd = -1;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].open) {
                continue;
            }
            p->fds[i].open = true;
            p->fds[i].data = data;
            p->fds[i].size = size;
            p->fds[i].pos  = 0;
            fd = i;
            break;
        }
    }

    irq_restore(flags);
    return fd;
}

bool process_fd_peek(int pid, int fd, const void **data, uint64_t *remaining) {
    uint64_t flags = irq_save();
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS && p->fds[fd].open) {
        struct fd *f = &p->fds[fd];
        if (data != NULL) {
            *data = f->data + f->pos;
        }
        if (remaining != NULL) {
            *remaining = f->size - f->pos;
        }
        ok = true;
    }

    irq_restore(flags);
    return ok;
}

void process_fd_advance(int pid, int fd, uint64_t n) {
    uint64_t flags = irq_save();

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS && p->fds[fd].open) {
        struct fd *f = &p->fds[fd];
        f->pos = (f->pos + n > f->size) ? f->size : f->pos + n;
    }

    irq_restore(flags);
}

bool process_fd_close(int pid, int fd) {
    uint64_t flags = irq_save();
    bool closed = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS && p->fds[fd].open) {
        p->fds[fd].open = false;
        closed = true;
    }

    irq_restore(flags);
    return closed;
}

size_t process_fd_count(int pid) {
    size_t n = 0;
    const struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].open) {
                n++;
            }
        }
    }
    return n;
}
