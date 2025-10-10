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
