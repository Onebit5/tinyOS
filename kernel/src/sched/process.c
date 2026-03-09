#include "sched/process.h"
#include "cpu/interrupts.h"
#include "sched/spinlock.h"
#include "lib/string.h"

/* a fixed table rather than a linked list of allocations. thirty-two is
 * more programs than this machine will ever usefully run at once, and a
 * fixed table has no lifetime questions at all -- which matters a great
 * deal for the one structure whose job is to outlive things */
/* the process table and every descriptor in it. ranked above the
 * scheduler because the scheduler reaches in here when a thread
 * dies, and nothing here ever calls back out */
static struct spinlock process_lock = SPINLOCK("process", LOCK_RANK_PROCESS);

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

const char *process_cwd(int pid) {
    struct process *p = slot_for(pid);
    return (p != NULL && p->cwd[0] != '\0') ? p->cwd : "/";
}

void process_set_cwd(int pid, const char *path) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        size_t i = 0;
        while (path[i] != '\0' && i < PATH_MAX - 1) {
            p->cwd[i] = path[i];
            i++;
        }
        p->cwd[i] = '\0';
    }
    spin_unlock_irq(&process_lock, flags);
}

int process_create(const char *name, int parent, int uid, bool announce,
                   uint64_t now_ms) {
    uint64_t flags = spin_lock_irq(&process_lock);
    int pid = 0;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0) {
            continue;
        }
        struct process *p = &table[i];

        p->pid        = next_pid++;
        p->parent     = parent;
        p->uid        = uid;
        p->announce   = announce;
        p->thread_id  = 0;
        p->exited     = false;
        p->exit_code  = 0;
        p->interrupted = false;
        p->started_ms = now_ms;
        p->ended_ms   = 0;

        /* wherever the parent was standing. a process started from a
         * directory should be in that directory, which is the whole
         * reason `cd` then running something behaves as anyone expects */
        const char *from = process_cwd(parent);
        size_t c = 0;
        while (from[c] != '\0' && c < PATH_MAX - 1) {
            p->cwd[c] = from[c];
            c++;
        }
        p->cwd[c] = '\0';
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

    spin_unlock_irq(&process_lock, flags);
    return pid;
}

void process_set_thread(int pid, int thread_id) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->thread_id = thread_id;
    }
    spin_unlock_irq(&process_lock, flags);
}

void process_exited(int pid, int code, uint64_t now_ms) {
    uint64_t flags = spin_lock_irq(&process_lock);
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
    spin_unlock_irq(&process_lock, flags);
}

bool process_collect(int pid, int *code) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool collected = false;

    struct process *p = slot_for(pid);
    if (p != NULL && p->exited) {
        if (code != NULL) {
            *code = p->exit_code;
        }
        p->pid = 0;         /* the slot is free again */
        collected = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return collected;
}

bool process_announces(int pid) {
    const struct process *p = slot_for(pid);
    return p != NULL && p->announce;
}

int process_uid(int pid) {
    const struct process *p = slot_for(pid);
    return (p != NULL) ? p->uid : -1;
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

/* ---- interrupts ----------------------------------------------------- */

void process_interrupt(int pid) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->interrupted = true;
    }
    spin_unlock_irq(&process_lock, flags);
}

bool process_interrupt_pending(int pid) {
    const struct process *p = slot_for(pid);
    return p != NULL && p->interrupted;
}

bool process_take_interrupt(int pid) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool had = false;
    struct process *p = slot_for(pid);
    if (p != NULL && p->interrupted) {
        p->interrupted = false;
        had = true;
    }
    spin_unlock_irq(&process_lock, flags);
    return had;
}

/* ---- open files ---------------------------------------------------- */

int process_fd_open(int pid, const void *data, uint64_t size) {
    uint64_t flags = spin_lock_irq(&process_lock);
    int fd = -1;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].open) {
                continue;
            }
            p->fds[i].open = true;
            p->fds[i].on_disk = false;
            p->fds[i].data = data;
            p->fds[i].cluster = 0;
            p->fds[i].size = size;
            p->fds[i].pos  = 0;
            fd = i;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return fd;
}

int process_fd_open_disk(int pid, uint32_t cluster, uint64_t size,
                         uint64_t entry_sector, uint32_t entry_offset) {
    uint64_t flags = spin_lock_irq(&process_lock);
    int fd = -1;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].open) {
                continue;
            }
            p->fds[i].open = true;
            p->fds[i].on_disk = true;
            p->fds[i].data = NULL;
            p->fds[i].cluster = cluster;
            p->fds[i].entry_sector = entry_sector;
            p->fds[i].entry_offset = entry_offset;
            p->fds[i].size = size;
            p->fds[i].pos  = 0;
            fd = i;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return fd;
}

bool process_fd_disk(int pid, int fd, struct fd_disk *out) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS
        && p->fds[fd].open && p->fds[fd].on_disk) {
        struct fd *f = &p->fds[fd];
        out->cluster = f->cluster;
        out->size = f->size;
        out->pos = f->pos;
        out->remaining = (f->pos < f->size) ? f->size - f->pos : 0;
        out->entry_sector = f->entry_sector;
        out->entry_offset = f->entry_offset;
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

void process_fd_grew(int pid, int fd, uint32_t cluster, uint64_t size) {
    uint64_t flags = spin_lock_irq(&process_lock);

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS
        && p->fds[fd].open && p->fds[fd].on_disk) {
        p->fds[fd].cluster = cluster;
        p->fds[fd].size = size;
    }

    spin_unlock_irq(&process_lock, flags);
}

bool process_fd_peek(int pid, int fd, const void **data, uint64_t *remaining) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS && p->fds[fd].open
        && !p->fds[fd].on_disk) {
        struct fd *f = &p->fds[fd];
        if (data != NULL) {
            *data = f->data + f->pos;
        }
        if (remaining != NULL) {
            *remaining = f->size - f->pos;
        }
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

void process_fd_advance(int pid, int fd, uint64_t n) {
    uint64_t flags = spin_lock_irq(&process_lock);

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS && p->fds[fd].open) {
        struct fd *f = &p->fds[fd];
        f->pos = (f->pos + n > f->size) ? f->size : f->pos + n;
    }

    spin_unlock_irq(&process_lock, flags);
}

bool process_fd_close(int pid, int fd) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool closed = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= FD_FIRST_FILE && fd < MAX_FDS && p->fds[fd].open) {
        p->fds[fd].open = false;
        closed = true;
    }

    spin_unlock_irq(&process_lock, flags);
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
