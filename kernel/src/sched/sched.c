#include "sched/sched.h"
#include "arch/cpu.h"
#include "sched/thread.h"
#include "drivers/pit.h"
#include "mm/pmm.h"
#include "mm/addrspace.h"
#include "sched/process.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "sched/spinlock.h"
#include "arch/context.h"
#include "mm/pmm.h"
#include "mm/addrspace.h"
#include "sched/process.h"
#include "fs/pipe.h"

/* how many ticks a thread gets before I take the cpu back. 5 ticks at
 * 100hz = 50ms, short enough to look instant, long enough that I am
 * not spending all my time switching */
#define QUANTUM_TICKS 5

/* implemented in switch.asm */

/* the run queue: a ring every core will eventually walk, and the
 * one place a thread can be in two states at once if nobody is
 * holding anything */
static struct spinlock sched_lock = SPINLOCK("sched", LOCK_RANK_SCHED);

/* the ring is shared: one queue that every core picks from, rather than
 * a queue each. with four cores the lock is not the bottleneck, and an
 * idle core taking whatever is ready *is* load balancing -- without the
 * migration machinery that per-core queues then need to put back what
 * they took apart.
 *
 * what is per core is which thread it is running, which idle thread it
 * falls back to, and how much of its slice is left. those cannot be
 * shared by definition */
struct percpu {
    /* not called `current`: there is a macro of that name just below,
     * so that the rest of this file reads as it always did, and a member
     * sharing the name would be rewritten out from under itself */
    struct thread *running;
    struct thread *idle;
    int quantum_left;
    bool scheduling;        /* has this core entered the scheduler yet */
};

static struct percpu percpu[CPU_MAX];

/* the ring itself, and any thread in it. `ring` is just a way in */
static struct thread *ring;

#define ME (&percpu[cpu_id()])
#define current (ME->running)

/* the thread that limine handed the cpu to. its stack came from the
 * bootloader rather than the pmm, which is why stack_phys stays 0 --
 * the reaper checks that before freeing anything */
static struct thread boot_thread;

struct thread *sched_current(void) {
    return current;
}

static void reap_dead(void);

static void idle_loop(void *arg) {
    (void)arg;
    /* the lowest form of life in the system. exists so there is always
     * someone to hand the cpu to when everybody else is asleep -- and,
     * now, so that clearing up after the dead has somewhere to happen
     * that is not inside the scheduler holding the run queue */
    for (;;) {
        reap_dead();
        cpu_idle();
    }
}

uint64_t sched_quantum_ms(void) {
    return QUANTUM_TICKS * (1000 / CLOCK_TICK_HZ);
}

void sched_add(struct thread *t) {
    /* the ring, not `current`. this used to ask whether *this core* had
     * a thread, which meant the same thing back when there was only one
     * core -- but a core building its own idle thread has no current
     * thread yet by definition, and that is precisely when it calls
     * this. the question was always meant to be "does the ring exist" */
    if (ring == NULL) {
        panic("sched_add before sched_init, the wheel hath no hub yet");
    }

    uint64_t flags = spin_lock_irq(&sched_lock);
    t->next = ring->next;
    ring->next = t;
    spin_unlock_irq(&sched_lock, flags);
}

/* free anything that has finished. I walk from current outward and
 * never touch current itself, so I am structurally incapable of
 * freeing the stack I am standing on */
/* dead threads, unlinked under the lock and freed outside it.
 *
 * two reasons it cannot be done in one breath any more. a thread marked
 * dead may still be *running* -- `kill` can mark one that is on another
 * core this instant -- and freeing the stack out from under a processor
 * that is standing on it is not a race, it is a crash. so nothing is
 * touched until its core has let it go.
 *
 * and the freeing itself takes the allocators' locks and tells every
 * other core to forget a mapping, which means waiting for an answer. a
 * core waiting for that answer while holding the run queue would be
 * waiting on cores that are waiting for the run queue */
static void reap_dead(void) {
    struct thread *dead[8];
    int n = 0;

    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *prev = current;
    struct thread *t = current->next;
    while (t != current && n < 8) {
        if (t->state == THREAD_DEAD && t->on_cpu < 0) {
            prev->next = t->next;
            dead[n++] = t;
            t = prev->next;
        } else {
            prev = t;
            t = t->next;
        }
    }
    ring = current;     /* whatever I unlinked, this is still in the ring */

    spin_unlock_irq(&sched_lock, flags);

    for (int i = 0; i < n; i++) {
        /* a thread that was *killed* never ran thread_exit, so this is
         * the only place its pipe ends get released. outside the lock,
         * because closing one wakes whoever is waiting on it */
        pipe_release_for(dead[i]->pid);
        thread_free_stack(dead[i]);
        thread_free(dead[i]);   /* a no-op for the boot thread */
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

/* is this thread one of the idle threads -- anybody's, not just mine?
 * an idle thread belongs to its own core and must never be picked up by
 * another, or two cores end up sharing one idle stack */
static bool is_idle(const struct thread *t) {
    for (size_t i = 0; i < CPU_MAX; i++) {
        if (percpu[i].idle == t) {
            return true;
        }
    }
    return false;
}

/* next in the ring who wants a cpu. idle is skipped on the walk and only
 * handed out when literally nobody else can use the core.
 *
 * the important word is READY. a thread another core is running is
 * marked RUNNING, and picking it up here would put two cores on one
 * stack -- which is not a race that corrupts something later, it is two
 * processors executing the same function with the same rsp */
static struct thread *pick_next(void) {
    struct thread *start = current != NULL ? current : ring;
    if (start == NULL) {
        return ME->idle;
    }

    struct thread *t = start->next;
    while (t != start) {
        if (!is_idle(t) && t->state == THREAD_READY && !t->stopped) {
            return t;
        }
        t = t->next;
    }

    if (current != NULL && current->state == THREAD_RUNNING
        && !current->stopped) {
        return current;     /* still runnable and nobody is waiting */
    }
    return ME->idle;
}

/* the actual switch. must be entered with interrupts off */
/* the run queue is rewritten here, so the lock has to be held on the way
 * in. it is *not* held on the way out by the thread that arrives: a
 * brand new one starts at thread_bootstrap and never returns through
 * this function at all, so it gives the lock back itself. see
 * sched_first_run */
static void schedule(void) {
    struct thread *prev = current;
    struct thread *next = pick_next();

    ME->quantum_left = QUANTUM_TICKS;

    if (next == prev) {
        prev->state = THREAD_RUNNING;
        prev->on_cpu = (int)cpu_id();
        return;
    }

    if (prev != NULL && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    if (prev != NULL) {
        prev->on_cpu = -1;
    }
    next->state = THREAD_RUNNING;
    next->on_cpu = (int)cpu_id();
    current = next;

    /* whose memory is real from here on. the kernel half is identical
     * in every space, so the stack I am standing on survives the
     * change -- that is the whole reason the upper half is shared */
    addrspace_switch(next->space);

    /* both of these say "where does the kernel stand when this thread
     * traps in from ring 3". the tss answers it for interrupts and the
     * syscall stub's own word answers it for `syscall`. both are per
     * core and both follow the thread -- two user threads landing on one
     * stack would eat each other, and so would two cores */
    if (next->stack_phys != 0) {
        uint64_t ktop = (uint64_t)pmm_phys_to_virt(next->stack_phys)
                      + next->stack_pages * PAGE_SIZE;
        context_set_kernel_stack(ktop);
    }

    switch_context(&prev->sp, &next->sp);
    /* when I get back here, an unknown amount of time has passed and
     * I am `prev` again. everything above is somebody elses story */
}

/* the first thing a newly created thread does.
 *
 * switching happens with the lock held, and normally the thread that
 * comes back releases it -- but a thread running for the first time
 * has no "comes back": it starts at the top of thread_bootstrap with
 * somebody else's lock in its hand and their release sitting on a stack
 * it will never return to. so it hands it back itself.
 *
 * getting this wrong looks like a keyboard interrupt panicking about
 * recursion, which is exactly how it was found */
void sched_first_run(void) {
    spin_unlock(&sched_lock);
}

void sched_yield(void) {
    uint64_t flags = spin_lock_irq(&sched_lock);
    schedule();
    spin_unlock_irq(&sched_lock, flags);
}

static void unlink(struct waitq *q, struct thread *t);

void waitq_enqueue(struct waitq *q) {
    uint64_t flags = spin_lock_irq(&sched_lock);
    current->wait_next = q->head;
    current->waiting_on = q;
    q->head = current;
    current->state = THREAD_BLOCKED;
    spin_unlock_irq(&sched_lock, flags);
}

void waitq_sleep(void) {
    uint64_t flags = spin_lock_irq(&sched_lock);

    /* if somebody woke me between enqueueing and getting here, I am
     * already READY and there is nothing to sleep through. that gap is
     * the whole reason these are two calls: it is what lets a caller
     * drop its own lock in the middle without losing the wakeup */
    if (current->state == THREAD_BLOCKED) {
        schedule();
    }
    current->waiting_on = NULL;

    spin_unlock_irq(&sched_lock, flags);
}

void waitq_block(struct waitq *q) {
    waitq_enqueue(q);
    waitq_sleep();
}

void waitq_wake_all(struct waitq *q) {
    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *t = q->head;
    while (t != NULL) {
        struct thread *next = t->wait_next;
        t->wait_next = NULL;
        t->waiting_on = NULL;
        if (t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
        }
        t = next;
    }
    q->head = NULL;

    spin_unlock_irq(&sched_lock, flags);
}

/* suspend, or let go again.
 *
 * a stopped thread keeps whatever it was doing -- it may be halfway
 * through a read, parked on a pipe, or simply ready -- and is only
 * removed from consideration. so it stops on the next tick rather than
 * this instant, which is at most one quantum and nobody can tell.
 *
 * that also means continuing it needs to do nothing but clear the flag.
 * whatever it was waiting for it is still waiting for, and if that
 * arrived while it was stopped it is already marked ready */
void sched_set_stopped(int id, bool stopped) {
    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *t = ring;
    if (t != NULL) {
        do {
            if (t->id == id) {
                t->stopped = stopped;
                break;
            }
            t = t->next;
        } while (t != ring);
    }

    spin_unlock_irq(&sched_lock, flags);
}

bool sched_thread_stopped(int id) {
    uint64_t flags = spin_lock_irq(&sched_lock);
    bool stopped = false;

    struct thread *t = ring;
    if (t != NULL) {
        do {
            if (t->id == id) {
                stopped = t->stopped && t->state != THREAD_DEAD;
                break;
            }
            t = t->next;
        } while (t != ring);
    }

    spin_unlock_irq(&sched_lock, flags);
    return stopped;
}

void sched_wake_thread(int id) {
    uint64_t flags = spin_lock_irq(&sched_lock);

    struct thread *t = current;
    do {
        if (t->id == id) {
            if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
                if (t->waiting_on != NULL) {
                    struct waitq *q = t->waiting_on;
                    t->waiting_on = NULL;
                    unlink(q, t);   /* the lock is already mine */
                }
                t->state = THREAD_READY;
            }
            break;
        }
        t = t->next;
    } while (t != current);

    spin_unlock_irq(&sched_lock, flags);
}

/* the guts, for callers already holding the lock. walking with a pointer
 * to the link rather than the node means removing the head needs no
 * special case */
static void unlink(struct waitq *q, struct thread *t) {
    struct thread **link = &q->head;
    while (*link != NULL) {
        if (*link == t) {
            *link = t->wait_next;
            t->wait_next = NULL;
            t->waiting_on = NULL;
            break;
        }
        link = &(*link)->wait_next;
    }
}

void waitq_remove(struct waitq *q, struct thread *t) {
    uint64_t flags = spin_lock_irq(&sched_lock);
    unlink(q, t);
    spin_unlock_irq(&sched_lock, flags);
}

void sleep_ms(uint64_t ms) {
    uint64_t ticks = (ms * PIT_HZ) / 1000;
    if (ticks == 0 && ms > 0) {
        ticks = 1;      /* asking for less than a tick still costs a tick */
    }

    uint64_t flags = spin_lock_irq(&sched_lock);
    current->wake_at = pit_ticks() + ticks;
    current->state = THREAD_SLEEPING;
    schedule();
    spin_unlock_irq(&sched_lock, flags);
}

void sched_tick(void) {
    if (current == NULL) {
        return;     /* timer beat the scheduler to it, nothing to do yet */
    }

    /* this walks the ring and may switch away, both of which are the
     * lock's business. it arrives from an interrupt with interrupts
     * already off, which used to be the whole of the protection */
    uint64_t flags = spin_lock_irq(&sched_lock);

    /* charge the tick to whoever was running when it arrived. the idle
     * thread is charged too -- time spent doing nothing is still time,
     * and seeing it is how you know the machine is mostly asleep */
    current->cpu_ticks++;

    wake_sleepers();

    if (--ME->quantum_left <= 0) {
        schedule();
    }

    spin_unlock_irq(&sched_lock, flags);
}

static void dump_one(struct thread *t) {
    kprintf("  %2d  %s", t->id, t->name);
    for (size_t i = strlen(t->name); i < THREAD_NAME_MAX; i++) {
        kprintf(" ");
    }
    /* stopped is not a state -- a suspended thread is still blocked on
     * whatever it was blocked on -- but it is what you want to see in
     * this column, because it is the reason it is not running */
    kprintf("%-9s", t->stopped ? "stopped" : thread_state_name(t->state));

    /* which core, if any, is running it this instant. a thread that is
     * merely ready is on nobody's cpu -- and with more than one core
     * that is the interesting column */
    if (t->on_cpu >= 0) {
        kprintf("%-6d", t->on_cpu);
    } else {
        kprintf("%-6s", "-");
    }

    /* what share of the ticks so far went to this one. the idle thread
     * usually holds most of them, which is the honest picture of a
     * machine waiting for somebody to type */
    uint64_t total = pit_ticks();
    uint64_t pct = (total > 0) ? (t->cpu_ticks * 100) / total : 0;
    kprintf("%3lu%% ", pct);

    if (t->pid != 0) {
        kprintf("  pid %d, %lu pages", t->pid, addrspace_frames(t->space));
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
    uint64_t flags = spin_lock_irq(&sched_lock);

    kprintf("threads\n");
    kprintf("  id  name             state    core  cpu  running\n");

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

    spin_unlock_irq(&sched_lock, flags);
}

size_t sched_thread_count(void) {
    uint64_t flags = spin_lock_irq(&sched_lock);
    size_t n = 0;
    struct thread *t = current;
    do { n++; t = t->next; } while (t != current);
    spin_unlock_irq(&sched_lock, flags);
    return n;
}

bool sched_thread_alive(int id) {
    uint64_t flags = spin_lock_irq(&sched_lock);
    bool alive = false;

    struct thread *t = current;
    do {
        if (t->id == id && t->state != THREAD_DEAD) {
            alive = true;
            break;
        }
        t = t->next;
    } while (t != current);

    spin_unlock_irq(&sched_lock, flags);
    return alive;
}

enum sched_kill_result sched_kill(int id) {
    uint64_t flags = spin_lock_irq(&sched_lock);
    enum sched_kill_result result = SCHED_KILL_NO_SUCH;

    struct thread *t = current;
    do {
        if (t->id == id) {
            if (t == current) {
                result = SCHED_KILL_SELF;
            } else if (is_idle(t) || t->pid == INIT_PID) {
                /* idle, because somebody has to be able to take the
                 * cpu -- and init, because it is what starts everything
                 * else and what collects everything else. a machine
                 * that has lost init still runs; it just cannot ever
                 * start another session or take itself down tidily,
                 * which is a worse state than being told no */
                result = SCHED_KILL_PROTECTED;
            } else if (t->state == THREAD_DEAD) {
                result = SCHED_KILL_NO_SUCH;    /* already gone */
            } else {
                /* if it is parked on a queue, take it off before the
                 * reaper frees it out from under that queue */
                if (t->waiting_on != NULL) {
                    struct waitq *q = t->waiting_on;
                    t->waiting_on = NULL;
                    unlink(q, t);   /* the lock is already mine */
                }
                if (t->pid != 0) {
                    process_exited(t->pid, PROCESS_KILLED, pit_uptime_ms());
                }
                t->state = THREAD_DEAD;
                result = SCHED_KILL_OK;
            }
            break;
        }
        t = t->next;
    } while (t != current);

    spin_unlock_irq(&sched_lock, flags);
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
    boot_thread.stack_phys = 0;     /* limine's, not mine to free */
    boot_thread.stack_pages = 0;
    boot_thread.next = &boot_thread;    /* a ring of one, for now */
    boot_thread.wait_next = NULL;
    boot_thread.cpu_ticks = 0;
    boot_thread.from_heap = false;      /* it lives in .bss */

    boot_thread.on_cpu = 0;

    ring = &boot_thread;
    current = &boot_thread;
    ME->quantum_left = QUANTUM_TICKS;
    ME->scheduling = true;

    ME->idle = thread_create("idle0", idle_loop, NULL);
    if (ME->idle == NULL) {
        panic("could not summon the idle thread. the wheel cannot turn");
    }
}

/* a core other than the first, joining in.
 *
 * it needs an idle thread of its own before it can enter the scheduler
 * at all -- the fallback when nothing is ready has to be a thread this
 * core alone is standing on, because two cores sharing an idle stack is
 * two cores sharing a stack.
 *
 * this is called on the core that will run it, so `current` and `idle`
 * below are that core's */
bool sched_join(unsigned cpu, const char *idle_name) {
    /* parked, so that between it going into the ring and this core
     * claiming it, no other core can pick it up. an idle thread belongs
     * to one core and two cores standing on one stack is the end of the
     * machine */
    struct thread *idle = thread_create_parked(idle_name, idle_loop, NULL);
    if (idle == NULL) {
        return false;
    }

    uint64_t flags = spin_lock_irq(&sched_lock);
    percpu[cpu].idle = idle;
    percpu[cpu].quantum_left = QUANTUM_TICKS;
    percpu[cpu].scheduling = true;

    /* claimed. it was parked so nobody else could take it; now it is
     * this core's, and this core is standing on it */
    idle->state = THREAD_RUNNING;
    idle->on_cpu = (int)cpu;
    percpu[cpu].running = idle;
    spin_unlock_irq(&sched_lock, flags);
    return true;
}

/* which core a thread is on, or -1. for `ps` */
int sched_thread_cpu(const struct thread *t) {
    return t->on_cpu;
}

const char *sched_cpu_running(unsigned cpu) {
    if (cpu >= CPU_MAX) {
        return "?";
    }
    struct thread *t = percpu[cpu].running;
    return (t != NULL) ? t->name : "nothing yet";
}

size_t sched_cores_scheduling(void) {
    size_t n = 0;
    for (size_t i = 0; i < CPU_MAX; i++) {
        if (percpu[i].scheduling) {
            n++;
        }
    }
    return n;
}
