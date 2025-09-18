#include "sched/sched.h"
#include "sched/thread.h"
#include "drivers/pit.h"
#include "mm/pmm.h"
#include "mm/addrspace.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "cpu/interrupts.h"
#include "cpu/tss.h"
#include "cpu/syscall.h"
#include "mm/pmm.h"
#include "mm/addrspace.h"

/* how many ticks a thread gets before we take the cpu back. 5 ticks at
 * 100hz = 50ms, short enough to look instant, long enough that we're
 * not spending all our time switching */
#define QUANTUM_TICKS 5

/* implemented in switch.asm */
extern void switch_context(uint64_t *save_rsp, uint64_t *load_rsp);

static struct thread *current;
static struct thread *idle_thread;
static int quantum_left;

/* the thread that limine handed the cpu to. its stack came from the
 * bootloader rather than the pmm, which is why stack_phys stays 0 --
 * the reaper checks that before freeing anything */
static struct thread boot_thread;

struct thread *sched_current(void) {
    return current;
}

static void idle_loop(void *arg) {
    (void)arg;
    /* the lowest form of life in the system. exists so theres always
     * someone to hand the cpu to when everybody else is asleep */
    for (;;) {
        asm volatile ("hlt");
    }
}

void sched_add(struct thread *t) {
    if (current == NULL) {
        panic("sched_add before sched_init, the wheel hath no hub yet");
    }
    uint64_t flags = irq_save();
    t->next = current->next;
    current->next = t;
    irq_restore(flags);
}

/* free anything that has finished. we walk from current outward and
 * never touch current itself, so we are structurally incapable of
 * freeing the stack we are standing on */
static void reap_dead(void) {
    struct thread *prev = current;
    struct thread *t = current->next;

    while (t != current) {
        if (t->state == THREAD_DEAD) {
            prev->next = t->next;
            thread_free_stack(t);
            if (t->from_heap) {
                kfree(t);   /* the boot thread is a global, leave it be */
            }
            t = prev->next;
        } else {
            prev = t;
            t = t->next;
        }
    }
}

static void wake_sleepers(void) {
    uint64_t now = pit_ticks();
    struct thread *t = current;
    do {
        if (t->state == THREAD_SLEEPING && now >= t->wake_at) {
            t->state = THREAD_READY;
        }
        t = t->next;
    } while (t != current);
}

/* next in the ring who wants the cpu. idle is skipped on the walk and
 * only handed out when literally nobody else can use it */
static struct thread *pick_next(void) {
    struct thread *t = current->next;

    while (t != current) {
        if (t != idle_thread && t->state == THREAD_READY) {
            return t;
        }
        t = t->next;
    }

    if (current->state == THREAD_RUNNING) {
        return current;     /* still runnable and nobody is waiting */
    }
    return idle_thread;
}

/* the actual switch. must be entered with interrupts off */
static void schedule(void) {
    reap_dead();

    struct thread *prev = current;
    struct thread *next = pick_next();

    quantum_left = QUANTUM_TICKS;

    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    next->state = THREAD_RUNNING;
    current = next;

    /* whose memory is real from here on. the kernel half is identical
     * in every space, so the stack we are standing on survives the
     * change -- that is the whole reason the upper half is shared */
    addrspace_switch(next->space);

    /* both of these say "where does the kernel stand when this thread
     * traps in from ring 3". the tss answers it for interrupts, the
     * global for `syscall`, and they must follow the thread or two
     * user threads would land on the same stack and eat each other */
    if (next->stack_phys != 0) {
        uint64_t ktop = (uint64_t)pmm_phys_to_virt(next->stack_phys)
                      + next->stack_pages * PAGE_SIZE;
        tss_set_rsp0(ktop);
        syscall_kernel_rsp = ktop;
    }

    switch_context(&prev->rsp, &next->rsp);
    /* when we get back here, an unknown amount of time has passed and
     * we are `prev` again. everything above is somebody elses story */
}

void sched_yield(void) {
    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void waitq_block(struct waitq *q) {
    /* interrupts are already off -- see the contract in sched.h.
     * we go on the queue and off the run queue in the same breath */
    current->wait_next = q->head;
    q->head = current;
    current->state = THREAD_BLOCKED;
    schedule();
    /* somebody woke us and the scheduler picked us back up */
}

void waitq_wake_all(struct waitq *q) {
    uint64_t flags = irq_save();

    struct thread *t = q->head;
    while (t != NULL) {
        struct thread *next = t->wait_next;
        t->wait_next = NULL;
        if (t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
        }
        t = next;
    }
    q->head = NULL;

    irq_restore(flags);
}

void sleep_ms(uint64_t ms) {
    uint64_t ticks = (ms * PIT_HZ) / 1000;
    if (ticks == 0 && ms > 0) {
        ticks = 1;      /* asking for less than a tick still costs a tick */
    }

    uint64_t flags = irq_save();
    current->wake_at = pit_ticks() + ticks;
    current->state = THREAD_SLEEPING;
    schedule();
    irq_restore(flags);
}

void sched_tick(void) {
    if (current == NULL) {
        return;     /* timer beat the scheduler to it, nothing to do yet */
    }

    wake_sleepers();

    if (--quantum_left <= 0) {
        schedule();
    }
}

static void dump_one(struct thread *t) {
    kprintf("  %2d  %s", t->id, t->name);
    for (size_t i = strlen(t->name); i < THREAD_NAME_MAX; i++) {
        kprintf(" ");
    }
    kprintf("%-9s", thread_state_name(t->state));

    /* a thread with an address space of its own is a program in ring 3
     * rather than a part of the kernel, and it is worth seeing which */
    if (t->space != NULL) {
        kprintf("  ring 3, %lu pages", addrspace_frames(t->space));
    } else {
        kprintf("  kernel");
    }
    if (t->state == THREAD_SLEEPING) {
        kprintf(" (%lu ticks)", t->wake_at > pit_ticks()
                                ? t->wake_at - pit_ticks() : 0);
    }
    if (t->stack_phys == 0) {
        kprintf("   (bootloader's)");
    }
    kprintf("\n");
}

void sched_dump(void) {
    uint64_t flags = irq_save();

    kprintf("  id  name             state    where\n");

    /* the ring is in newest-first order, because sched_add splices each
     * new thread in just after current. that is fine for scheduling and
     * confusing to read, so print by id instead. a sweep per id is
     * quadratic and there are five threads */
    int highest = 0;
    for (struct thread *t = current; ; ) {
        if (t->id > highest) {
            highest = t->id;
        }
        t = t->next;
        if (t == current) break;
    }

    for (int want = 0; want <= highest; want++) {
        for (struct thread *t = current; ; ) {
            if (t->id == want) {
                dump_one(t);
                break;
            }
            t = t->next;
            if (t == current) break;
        }
    }

    irq_restore(flags);
}

size_t sched_thread_count(void) {
    uint64_t flags = irq_save();
    size_t n = 0;
    struct thread *t = current;
    do { n++; t = t->next; } while (t != current);
    irq_restore(flags);
    return n;
}

bool sched_thread_alive(int id) {
    uint64_t flags = irq_save();
    bool alive = false;

    struct thread *t = current;
    do {
        if (t->id == id && t->state != THREAD_DEAD) {
            alive = true;
            break;
        }
        t = t->next;
    } while (t != current);

    irq_restore(flags);
    return alive;
}

enum sched_kill_result sched_kill(int id) {
    uint64_t flags = irq_save();
    enum sched_kill_result result = SCHED_KILL_NO_SUCH;

    struct thread *t = current;
    do {
        if (t->id == id) {
            if (t == current) {
                result = SCHED_KILL_SELF;
            } else if (t == idle_thread) {
                result = SCHED_KILL_PROTECTED;
            } else if (t->state == THREAD_BLOCKED) {
                result = SCHED_KILL_BLOCKED;
            } else if (t->state == THREAD_DEAD) {
                result = SCHED_KILL_NO_SUCH;    /* already gone */
            } else {
                t->state = THREAD_DEAD;
                result = SCHED_KILL_OK;
            }
            break;
        }
        t = t->next;
    } while (t != current);

    irq_restore(flags);
    return result;
}

void sched_init(void) {
    boot_thread.id = 0;
    boot_thread.name[0] = 'b';
    boot_thread.name[1] = 'o';
    boot_thread.name[2] = 'o';
    boot_thread.name[3] = 't';
    boot_thread.name[4] = '\0';
    boot_thread.state = THREAD_RUNNING;
    boot_thread.stack_phys = 0;     /* limine's, not ours to free */
    boot_thread.stack_pages = 0;
    boot_thread.next = &boot_thread;    /* a ring of one, for now */
    boot_thread.wait_next = NULL;
    boot_thread.from_heap = false;      /* it lives in .bss */

    current = &boot_thread;
    quantum_left = QUANTUM_TICKS;

    idle_thread = thread_create("idle", idle_loop, NULL);
    if (idle_thread == NULL) {
        panic("could not summon the idle thread. the wheel cannot turn");
    }
}
