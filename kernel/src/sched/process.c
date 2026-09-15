#include "sched/process.h"
#include "cpu/interrupts.h"
#include "sched/spinlock.h"
#include "fs/pipe.h"
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
        /* its own group until somebody says otherwise, which is what a
         * command typed on its own is: a job of one */
        p->pgid       = p->pid;
        p->uid        = uid;
        p->announce   = announce;
        p->thread_id  = 0;
        p->exited     = false;
        p->exit_code  = 0;
        p->interrupted = false;
        p->started_ms = now_ms;
        p->ended_ms   = 0;

        /* the three every process is born with. a program that never
         * thinks about descriptors still reads the keyboard and prints
         * to the screen, because those are already sitting in 0, 1 and
         * 2 before it starts */
        memset(p->fds, 0, sizeof p->fds);
        p->fds[FD_STDIN].kind  = FD_KEYBOARD;
        p->fds[FD_STDOUT].kind = FD_CONSOLE;
        p->fds[FD_STDERR].kind = FD_CONSOLE;

        /* an empty environment rather than no environment. the
         * difference matters to every walk below, which stops at the
         * first empty string */
        p->env[0] = '\0';
        p->env_len = 1;

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
            /* a pipe sitting in one is *not* released here: closing one
             * wakes threads, and this is holding the process table.
             * whoever took the pipes off it beforehand does that part */
            p->fds[f].kind = FD_FREE;
        }

        /* and whatever it was the parent of is init's now.
         *
         * this is the moment, and the only moment: afterwards the slot
         * is collected and there is nothing left to say who these were
         * the children of. a child left pointing at a dead parent is a
         * child nobody will ever wait for, holding a slot forever --
         * which is what "a first process that owns the others" is
         * actually for. it is not a hierarchy for its own sake, it is
         * the answer to who collects you when your parent does not */
        for (size_t j = 0; j < MAX_PROCESSES; j++) {
            if (table[j].pid != 0 && table[j].parent == pid) {
                table[j].parent = INIT_PID;
            }
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

/* ---- what init owns -------------------------------------------------- */

/* is there still a process with this pid? asked of the *table* rather
 * than of anything above it, because a parent that has exited but not
 * been collected is still here and can still be waited for */
static bool present(int pid) {
    return pid != 0 && slot_for(pid) != NULL;
}

int process_orphan(void) {
    uint64_t flags = spin_lock_irq(&process_lock);
    int pid = 0;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &table[i];
        if (p->pid == 0 || !p->exited) {
            continue;
        }
        /* parent 0 is a kernel shell's, and the shell collects its own.
         * asked first and on its own, because 0 is also not a pid that
         * is *in* the table -- so the "nobody is left to wait for this"
         * test below says yes to every one of them unless 0 is taken out
         * of its way first.
         *
         * this is also what keeps init out of its own list, without a
         * second check that reads like defence and could never fire:
         * init's parent is 0, because init is what kmain started */
        if (p->parent == 0) {
            continue;
        }
        /* whose parent is init, or whose parent is not in the table at
         * all, has nobody else who could */
        if (p->parent == INIT_PID || !present(p->parent)) {
            pid = p->pid;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return pid;
}

size_t process_interrupt_all(void) {
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid == 0 || table[i].exited
            || table[i].pid == INIT_PID) {
            continue;
        }
        table[i].interrupted = true;
        n++;
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
}

size_t process_running_threads(int *ids, size_t max) {
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    for (size_t i = 0; i < MAX_PROCESSES && n < max; i++) {
        if (table[i].pid == 0 || table[i].exited
            || table[i].pid == INIT_PID || table[i].thread_id == 0) {
            continue;
        }
        ids[n++] = table[i].thread_id;
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
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

/* ---- groups --------------------------------------------------------- */

int process_pgid(int pid) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    int g = (p != NULL) ? p->pgid : -1;
    spin_unlock_irq(&process_lock, flags);
    return g;
}

void process_set_pgid(int pid, int pgid) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        p->pgid = pgid;
    }
    spin_unlock_irq(&process_lock, flags);
}

size_t process_group_threads(int pgid, int *ids, size_t max) {
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    for (size_t i = 0; i < MAX_PROCESSES && n < max; i++) {
        if (table[i].pid == 0 || table[i].pgid != pgid) {
            continue;
        }
        if (table[i].exited || table[i].thread_id == 0) {
            continue;
        }
        ids[n++] = table[i].thread_id;
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
}

bool process_group_alive(int pgid) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool alive = false;

    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0 && table[i].pgid == pgid && !table[i].exited) {
            alive = true;
            break;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return alive;
}

void process_interrupt_group(int pgid) {
    uint64_t flags = spin_lock_irq(&process_lock);
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].pid != 0 && table[i].pgid == pgid && !table[i].exited) {
            table[i].interrupted = true;
        }
    }
    spin_unlock_irq(&process_lock, flags);
}

/* ---- open files ---------------------------------------------------- */

int process_fd_open(int pid, const void *data, uint64_t size) {
    uint64_t flags = spin_lock_irq(&process_lock);
    int fd = -1;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = FD_FIRST_FILE; i < MAX_FDS; i++) {
            if (p->fds[i].kind != FD_FREE) {
                continue;
            }
            memset(&p->fds[i], 0, sizeof p->fds[i]);
            p->fds[i].kind = FD_MEMORY;
            p->fds[i].data = data;
            p->fds[i].size = size;
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
            if (p->fds[i].kind != FD_FREE) {
                continue;
            }
            memset(&p->fds[i], 0, sizeof p->fds[i]);
            p->fds[i].kind = FD_DISK;
            p->fds[i].cluster = cluster;
            p->fds[i].entry_sector = entry_sector;
            p->fds[i].entry_offset = entry_offset;
            p->fds[i].size = size;
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
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind == FD_DISK) {
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
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind == FD_DISK) {
        p->fds[fd].cluster = cluster;
        p->fds[fd].size = size;
    }

    spin_unlock_irq(&process_lock, flags);
}

bool process_fd_peek(int pid, int fd, const void **data, uint64_t *remaining) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind == FD_MEMORY) {
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
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind != FD_FREE) {
        /* clamped to the size, which is right for both directions: a
         * read cannot go past the end, and a write has already had the
         * new size recorded by process_fd_grew before this runs */
        struct fd *f = &p->fds[fd];
        f->pos = (f->pos + n > f->size) ? f->size : f->pos + n;
    }

    spin_unlock_irq(&process_lock, flags);
}

/* ---- the environment -------------------------------------------------
 *
 * the block itself is pure arithmetic over a run of strings and lives
 * in lib/env.c, because the shell needs exactly the same operations and
 * has no process to perform them on -- it is a kernel thread, so its
 * environment lives in its session. one implementation, two callers, and
 * no chance of them disagreeing about what "already set" means */

bool process_set_env(int pid, const char *block, size_t len) {
    if (len > PROC_ENV_MAX) {
        return false;
    }
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    if (p != NULL) {
        memcpy(p->env, block, len);
        p->env_len = len;
    }
    spin_unlock_irq(&process_lock, flags);
    return p != NULL;
}

size_t process_get_env(int pid, char *out, size_t max) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    size_t n = 0;
    if (p != NULL && p->env_len <= max) {
        memcpy(out, p->env, p->env_len);
        n = p->env_len;
    }
    spin_unlock_irq(&process_lock, flags);

    if (n == 0 && max > 0) {
        out[0] = '\0';     /* an empty environment is still an environment */
        n = 1;
    }
    return n;
}

bool process_env_get(int pid, const char *name, char *out, size_t max) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    bool ok = false;
    if (p != NULL) {
        ok = env_block_get(p->env, p->env_len, name, out, max);
    }
    spin_unlock_irq(&process_lock, flags);
    return ok;
}

bool process_env_set(int pid, const char *name, const char *value) {
    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *p = slot_for(pid);
    bool ok = false;
    if (p != NULL) {
        if (p->env_len == 0) {
            p->env[0] = '\0';
            p->env_len = 1;
        }
        ok = env_block_set(p->env, &p->env_len, PROC_ENV_MAX, name, value);
    }
    spin_unlock_irq(&process_lock, flags);
    return ok;
}

const char *process_name(int pid) {
    const struct process *p = slot_for(pid);
    return (p != NULL) ? p->name : "?";
}

bool process_fds_inherit(int pid, int from) {
    /* copied out under the lock and the pipes referenced after it.
     * taking a pipe's lock while holding this one is the wrong way up
     * the ranks, and a pipe wakes threads besides */
    struct fd copy[MAX_FDS];

    uint64_t flags = spin_lock_irq(&process_lock);
    struct process *child = slot_for(pid);
    struct process *parent = slot_for(from);
    bool ok = (child != NULL && parent != NULL);
    if (ok) {
        for (int i = 0; i < MAX_FDS; i++) {
            copy[i] = parent->fds[i];
            child->fds[i] = copy[i];
        }
    }
    spin_unlock_irq(&process_lock, flags);

    if (!ok) {
        return false;
    }
    for (int i = 0; i < MAX_FDS; i++) {
        if (copy[i].kind == FD_PIPE) {
            pipe_share(copy[i].pipe, copy[i].writing);
        }
    }
    return true;
}

bool process_fd_get(int pid, int fd, struct fd *out) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind != FD_FREE) {
        *out = p->fds[fd];
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

bool process_fd_install(int pid, int fd, const struct fd *src) {
    uint64_t flags = spin_lock_irq(&process_lock);
    bool ok = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS) {
        p->fds[fd] = *src;
        ok = true;
    }

    spin_unlock_irq(&process_lock, flags);
    return ok;
}

size_t process_take_pipes(int pid, struct pipe_end *out, size_t max) {
    uint64_t flags = spin_lock_irq(&process_lock);
    size_t n = 0;

    struct process *p = slot_for(pid);
    if (p != NULL) {
        for (int i = 0; i < MAX_FDS && n < max; i++) {
            if (p->fds[i].kind != FD_PIPE) {
                continue;
            }
            out[n].p = p->fds[i].pipe;
            out[n].writing = p->fds[i].writing;
            n++;
            p->fds[i].kind = FD_FREE;
            p->fds[i].pipe = NULL;
        }
    }

    spin_unlock_irq(&process_lock, flags);
    return n;
}

bool process_fd_close(int pid, int fd, struct pipe_end *closing) {
    if (closing != NULL) {
        closing->p = NULL;
    }
    uint64_t flags = spin_lock_irq(&process_lock);
    bool closed = false;

    struct process *p = slot_for(pid);
    if (p != NULL && fd >= 0 && fd < MAX_FDS && p->fds[fd].kind != FD_FREE) {
        /* a pipe cannot be let go of here -- that wakes threads, and
         * this is holding the table. it is handed back instead */
        if (p->fds[fd].kind == FD_PIPE && closing != NULL) {
            closing->p = p->fds[fd].pipe;
            closing->writing = p->fds[fd].writing;
        }
        p->fds[fd].kind = FD_FREE;
        p->fds[fd].pipe = NULL;
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
            if (p->fds[i].kind != FD_FREE) {
                n++;
            }
        }
    }
    return n;
}
