#include "sched/init.h"
#include "lib/string.h"

/* ---- the policy, which starts nothing ---------------------------------
 *
 * everything above the #ifndef is arithmetic over a table and a clock
 * reading. that is on purpose: the interesting decisions init makes are
 * about *repetition* -- has this died before, how long ago, is that a
 * loop or a coincidence -- and a decision you can only reach by letting
 * a real service die five times on a real machine is a decision nobody
 * ever checks. */

void init_table_reset(struct init_table *t) {
    memset(t, 0, sizeof *t);
}

bool init_add(struct init_table *t, const char *name, void (*entry)(void *),
              void *arg, unsigned console, bool respawn) {
    if (t->count >= INIT_SERVICES_MAX) {
        return false;
    }
    struct service *s = &t->s[t->count];
    memset(s, 0, sizeof *s);

    size_t i = 0;
    while (name[i] != '\0' && i < INIT_NAME_MAX - 1) {
        s->name[i] = name[i];
        i++;
    }
    s->name[i]  = '\0';
    s->entry    = entry;
    s->arg      = arg;
    s->console  = console;
    s->respawn  = respawn;
    s->state    = SERVICE_STOPPED;

    t->count++;
    return true;
}

void init_started(struct init_table *t, size_t i, int tid, uint64_t now_ms) {
    if (i >= t->count) {
        return;
    }
    struct service *s = &t->s[i];
    s->state     = SERVICE_RUNNING;
    s->thread_id = tid;
    s->starts++;

    /* the window opens on the *first* start of a run, not on every one.
     * opening it here each time would mean a service that dies and
     * restarts in ten milliseconds keeps resetting the very window that
     * is supposed to catch it doing exactly that */
    if (s->in_window == 0) {
        s->window_start_ms = now_ms;
    }
    s->in_window++;
}

enum init_action init_died(struct init_table *t, size_t i, uint64_t now_ms) {
    if (i >= t->count) {
        return INIT_LEAVE;
    }
    struct service *s = &t->s[i];
    s->state     = SERVICE_STOPPED;
    s->thread_id = 0;

    if (!s->respawn) {
        return INIT_LEAVE;
    }

    /* deaths spread out are not a loop.
     *
     * somebody logging out five times over an afternoon and somebody's
     * login prompt crashing five times a second are the same *count*
     * and are not remotely the same event. the window is what tells
     * them apart, and without it the machine would give up on a console
     * for the crime of having been used */
    if (now_ms - s->window_start_ms > INIT_RESPAWN_WINDOW_MS) {
        s->in_window = 0;
        s->window_start_ms = now_ms;
        return INIT_RESTART;
    }

    if (s->in_window > INIT_RESPAWN_MAX) {
        s->state = SERVICE_GIVEN_UP;
        return INIT_GIVE_UP;
    }
    return INIT_RESTART;
}

bool init_revive(struct init_table *t, size_t i) {
    if (i >= t->count || t->s[i].state == SERVICE_RUNNING) {
        return false;
    }
    struct service *s = &t->s[i];
    s->state     = SERVICE_STOPPED;
    s->respawn   = true;

    /* a full allowance again. without this the first thing a revived
     * service does is get given up on, since it is still carrying the
     * count that got it disabled in the first place */
    s->in_window = 0;
    s->window_start_ms = 0;
    return true;
}

int init_find(const struct init_table *t, const char *name) {
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->s[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const char *init_state_name(enum service_state s) {
    switch (s) {
    case SERVICE_RUNNING:   return "running";
    case SERVICE_GIVEN_UP:  return "given up";
    case SERVICE_STOPPED:   break;
    }
    return "stopped";
}

/* ---- the machinery ---------------------------------------------------- */

#ifndef TINYOS_HOSTED

#include "sched/sched.h"
#include "sched/thread.h"
#include "sched/spinlock.h"
#include "shell/shell.h"
#include "drivers/console.h"
#include "drivers/pit.h"
#include "drivers/tty.h"
#include "arch/machine.h"
#include "arch/cpu.h"
#include "fs/disk.h"
#include "fs/pipe.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"

/* how often init looks around. a service dying is not urgent -- nothing
 * is waiting on the answer except the person who will sit down at that
 * console next -- and a supervisor that spins is a supervisor that costs
 * more than the things it supervises. anything that *is* urgent wakes
 * this thread directly */
#define INIT_TICK_MS   400

/* how long the machine waits for programs to notice it is going down,
 * before ending them itself. an interrupt is not an order here: it is a
 * flag a process finds on its next syscall, so a program in a tight loop
 * that asks the kernel for nothing will never see it. that program still
 * has to stop */
#define INIT_GRACE_MS  1000

/* the table is written by init's thread and read by whichever shell was
 * asked to print it. the lock is taken around the table itself and never
 * across anything that could take another lock -- init starts threads
 * and kills them with this *not* held, which is the only discipline that
 * keeps a supervisor out of the ranking argument entirely */
static struct spinlock init_lock = SPINLOCK("init", LOCK_RANK_DEVICE);
static struct init_table services;

static int init_tid;

/* a shutdown that has been asked for and not yet acted on, and where it
 * was asked from -- so the machine says goodbye on the screen the person
 * is looking at rather than on whichever one init happens to be on */
static volatile bool     stop_pending;
static volatile int      stop_how;
static volatile unsigned stop_console;

bool init_stopping(void) { return stop_pending; }

/* ---- the services ---------------------------------------------------- */

/* a console session. it returns when somebody logs out, and init starts
 * a fresh one -- which is the point, and is a real difference rather
 * than a tidier way of writing the same thing. `logout` used to call
 * `login` from inside the running session, so the next person inherited
 * the last one's working directory, history, jobs and variables. a
 * session that *ends* leaves nothing behind, because there is nothing
 * left to leave it in */
static void console_service(void *arg) {
    (void)arg;
    shell_run();
    thread_exit(0);
}

/* the disk, kept roughly honest.
 *
 * a write-back cache means what is on the drive lags what the machine
 * believes, and `sync` is how somebody closes that gap on purpose. this
 * closes it on a timer instead, so the gap has a *size*: a few seconds
 * of work rather than however long since the last time anybody thought
 * about it.
 *
 * it is not a replacement for sync and does not pretend to be. it turns
 * "you might lose anything" into "you might lose the last few seconds",
 * which is the difference between a machine you cannot trust and one you
 * should still type sync at before pulling the plug */
#define FLUSH_EVERY_MS 3000

static void flusher_service(void *arg) {
    (void)arg;
    for (;;) {
        sleep_ms(FLUSH_EVERY_MS);
        if (disk_ready() && disk_dirty()) {
            (void)disk_sync();
        }
    }
}

/* ---- starting and stopping them -------------------------------------- */

/* start service `i`. the thread is created *parked* and released after
 * it has been told which console it belongs to: a thread inherits the
 * console of whoever made it, and everything here is made by init, so a
 * session that ran for even one instruction before being moved would
 * print its first line on init's screen instead of its own */
static bool start_service(size_t i) {
    struct service copy;

    uint64_t flags = spin_lock_irq(&init_lock);
    if (i >= services.count || services.s[i].state == SERVICE_RUNNING) {
        spin_unlock_irq(&init_lock, flags);
        return false;
    }
    /* claimed before the thread exists, and that ordering is the whole
     * of it. init's own sweep runs on init's thread and `init start`
     * runs on whichever shell typed it, so on a machine with four cores
     * they can both decide to start the same service in the same
     * instant -- and the result is two sessions on one console, which is
     * precisely the bug four consoles were meant to be the end of.
     *
     * the thread id goes to zero with it, because for the moment there
     * genuinely is no thread, and the sweep below has to be able to tell
     * that from a thread that has died */
    services.s[i].state = SERVICE_RUNNING;
    services.s[i].thread_id = 0;
    copy = services.s[i];
    spin_unlock_irq(&init_lock, flags);

    struct thread *t = thread_create_parked(copy.name, copy.entry, copy.arg);
    if (t == NULL) {
        flags = spin_lock_irq(&init_lock);
        services.s[i].state = SERVICE_STOPPED;      /* the claim, given back */
        spin_unlock_irq(&init_lock, flags);
        return false;
    }
    t->console = copy.console;

    flags = spin_lock_irq(&init_lock);
    init_started(&services, i, t->id, pit_uptime_ms());
    spin_unlock_irq(&init_lock, flags);

    sched_wake_thread(t->id);
    return true;
}

static void stop_service(size_t i) {
    uint64_t flags = spin_lock_irq(&init_lock);
    int tid = services.s[i].thread_id;
    services.s[i].respawn = false;
    services.s[i].state = SERVICE_STOPPED;
    services.s[i].thread_id = 0;
    spin_unlock_irq(&init_lock, flags);

    if (tid != 0) {
        sched_kill(tid);
    }
}

/* move init's own output to a given screen, and say where it was.
 *
 * init lives on console 1, and a message about console 3 belongs on
 * console 3 -- where somebody is sitting in front of a blank screen
 * wondering what happened to it. output belongs to its writer here, so
 * the only way to write somewhere else is to briefly be somewhere else */
static unsigned speak_on(unsigned console) {
    struct thread *me = sched_current();
    if (me == NULL) {
        return console;
    }
    unsigned was = me->console;
    me->console = console;
    return was;
}

/* every service that is not running, brought back if it should be.
 *
 * "is it running" is asked of the scheduler rather than remembered here,
 * because a thread can end in ways that never come back through init:
 * killed from another console, or ended by a fault. the table remembers
 * what the scheduler cannot -- how often, and how recently */
static void supervise(void) {
    size_t count;
    uint64_t flags = spin_lock_irq(&init_lock);
    count = services.count;
    spin_unlock_irq(&init_lock, flags);

    for (size_t i = 0; i < count; i++) {
        flags = spin_lock_irq(&init_lock);
        bool running = services.s[i].state == SERVICE_RUNNING;
        int  tid     = services.s[i].thread_id;
        spin_unlock_irq(&init_lock, flags);

        /* a zero thread id is a start that has been claimed and not yet
         * finished. it is *not* a service that has died, and treating it
         * as one would have init restart something it is in the middle
         * of starting */
        if (!running || tid == 0 || sched_thread_alive(tid)) {
            continue;
        }

        flags = spin_lock_irq(&init_lock);
        enum init_action what = init_died(&services, i, pit_uptime_ms());
        char name[INIT_NAME_MAX];
        memcpy(name, services.s[i].name, sizeof name);
        unsigned tries   = services.s[i].in_window;
        unsigned console = services.s[i].console;
        spin_unlock_irq(&init_lock, flags);

        if (what == INIT_RESTART) {
            if (!start_service(i)) {
                unsigned was = speak_on(console);
                kprintf("init: no memory to bring %s back. it stays down\n",
                        name);
                speak_on(was);
            }
        } else if (what == INIT_GIVE_UP) {
            /* the message matters as much as the rule, and where it is
             * printed matters as much as the message. a console that has
             * quietly stopped coming back looks like a broken machine;
             * one that says why, on itself, looks like a machine that
             * noticed and is telling the person sitting in front of it */
            unsigned was = speak_on(console);
            kprintf("\ninit: %s has died %u times in under %u seconds. "
                    "leaving it down --\n"
                    "      `init start %s` from another console to try "
                    "again\n",
                    name, tries, INIT_RESPAWN_WINDOW_MS / 1000, name);
            speak_on(was);
        }
    }
}

bool init_restart(const char *name) {
    uint64_t flags = spin_lock_irq(&init_lock);
    int i = init_find(&services, name);
    bool ok = (i >= 0) && init_revive(&services, (size_t)i);
    spin_unlock_irq(&init_lock, flags);

    if (ok) {
        ok = start_service((size_t)i);
    }
    return ok;
}

void init_snapshot(struct init_table *out) {
    uint64_t flags = spin_lock_irq(&init_lock);
    *out = services;
    spin_unlock_irq(&init_lock, flags);
}

/* ---- taking the machine down ----------------------------------------- */

void init_stop_machine(enum init_stop how) {
    stop_how     = (int)how;
    stop_console = tty_my_console();
    stop_pending = true;
    sched_wake_thread(init_tid);

    /* and whoever asked is finished. it must not go back to its prompt:
     * a shell printing `igor@velvet:/#` over the top of the shutdown
     * looks exactly like a machine that ignored the command, and the
     * one thing it could usefully do next is nothing at all */
    for (;;) {
        sleep_ms(1000);
    }
}

/* ask everything still running to stop, and then insist.
 *
 * asking first is worth the second it costs: an interrupted program gets
 * to return an error from whatever syscall it was in and unwind, which
 * for anything holding a file means the write it was part-way through
 * either happens or does not. killed threads leave that half-done */
static void stop_the_programs(void) {
    size_t running = process_interrupt_all();
    if (running == 0) {
        return;
    }

    kprintf("init: asking %lu program%s to stop\n",
            running, running == 1 ? "" : "s");

    for (uint64_t waited = 0; waited < INIT_GRACE_MS; waited += 100) {
        sleep_ms(100);
        if (process_interrupt_all() == 0) {
            return;
        }
    }

    int ids[MAX_PROCESSES];
    size_t n = process_running_threads(ids, MAX_PROCESSES);
    if (n > 0) {
        kprintf("init: %lu did not answer. ending %s\n",
                n, n == 1 ? "it" : "them");
        for (size_t i = 0; i < n; i++) {
            sched_kill(ids[i]);
        }
    }
}

/* the last thing on the screen.
 *
 * this used to live in `system.c` next to the code that pulses the reset
 * line, which put a persona quote in the architecture layer -- it is not
 * x86 and never was, it is what *this kernel* says when it stops. so it
 * lives with the thing that decides to stop, and what is left behind the
 * arch boundary is three pokes at a motherboard.
 *
 * interrupts stay on through all of it, because the wait counts timer
 * ticks and the timer cannot tick with them off. the door is shut only
 * once there is nothing left to wait for */
static void say_goodbye(enum init_stop how) {
    console_set_colors(0x7b8ce0, 0x101018);

    if (how == INIT_REBOOT) {
        kprintf("\nThou art I... And I am thou...\n");
        kprintf("Thou hast established a genuine bond...\n\n");
        kprintf("The innermost power of the Computer\n");
        kprintf("Arcana hath been set free.\n\n");
        kprintf("I bestow upon thee the ability to\n");
        kprintf("create tinyOS, the ultimate form\n");
        kprintf("of the Computer's Arcana...\n\n");
        pit_busy_wait(3000);
    } else {
        kprintf("\nThe Velvet Room fades...\n");
        kprintf("Till I meet again.\n");
        pit_busy_wait(1500);
    }
}

static void take_the_machine_down(void) {
    enum init_stop how = (enum init_stop)stop_how;

    /* say it where it was asked. init lives on console 1 and the person
     * who typed this may well be on console 3, and a machine that says
     * goodbye on a screen nobody is looking at has said nothing */
    struct thread *me = sched_current();
    if (me != NULL) {
        me->console = stop_console;
    }

    kprintf("\ninit: going down%s\n",
            how == INIT_REBOOT ? " for a reboot" : "");

    stop_the_programs();

    /* the services, in the reverse of the order they came up.
     *
     * reverse is not symmetry for its own sake. the sessions are what
     * can still write to the disk, and the flusher is what protects it
     * -- so the sessions go first and the thing looking after the disk
     * is the last one standing, which is the same order every system
     * that has ever had a shutdown script uses */
    size_t count;
    uint64_t flags = spin_lock_irq(&init_lock);
    count = services.count;
    spin_unlock_irq(&init_lock, flags);

    for (size_t i = count; i-- > 0;) {
        stop_service(i);
    }

    /* and only now, with nothing left that could add to it.
     *
     * this is the whole reason shutdown belongs to init rather than to
     * the `reboot` command. the command syncs and resets from whichever
     * console typed it, while three other sessions are still running and
     * still able to write -- so what reaches the drive is whatever was
     * dirty at the moment one console asked, and anything the others did
     * in the meantime is gone */
    if (disk_ready() && disk_dirty()) {
        kprintf("init: writing what is still in memory...\n");
        if (!disk_sync()) {
            kprintf("init: the drive refused. something is being lost here\n");
        }
    }

    say_goodbye(how);

    if (how == INIT_REBOOT) {
        machine_reset();        /* which does not come back either way */
    }

    /* and a poweroff that nothing answered is worth saying out loud. a
     * machine sitting there with the fan running looks broken; "close
     * the window" is a complete answer and takes one line */
    machine_poweroff();
    kprintf("init: nothing answered. halting instead -- close the window\n");
    cpu_stop();
}

/* ---- process 1 -------------------------------------------------------- */

static void init_thread(void *arg) {
    (void)arg;

    /* the loader's memory, handed back before anything else exists.
     *
     * this used to be the first shell's job, which was true of nothing
     * except the order things happened to be written in -- reclaiming
     * the memory philemon left behind is a step in bringing the machine
     * up, and a login prompt is not the thing that should be deciding
     * when it happens */
    uint64_t gained = pmm_reclaim_bootloader();
    kprintf("reclaimed %lu KiB of bootloader memory (%lu MiB usable now)\n",
            gained / 1024, pmm_total_bytes() / (1024 * 1024));
    kprintf("four consoles: alt+1..4 (or alt+f1..f4, if your host does "
            "not eat them),\n");
    kprintf("             ctrl+\\ then a digit on serial, or `chvt`. "
            "shift+pageup looks back\n");

    /* the order below is the order the machine comes up in, and it is
     * the reverse of the order it goes down in.
     *
     * the flusher first, because it is what stands between a
     * write-back cache and a machine that loses the last thing anybody
     * typed -- and it should be there before there is anybody able to
     * type. the sessions after it, so the last thing to appear on a
     * screen is a prompt rather than a service report */
    uint64_t flags = spin_lock_irq(&init_lock);
    init_table_reset(&services);
    init_add(&services, "flusher", flusher_service, NULL, 0, true);
    for (unsigned c = 0; c < VCONSOLE_COUNT; c++) {
        char name[INIT_NAME_MAX];
        name[0] = 't'; name[1] = 't'; name[2] = 'y';
        name[3] = (char)('1' + c); name[4] = '\0';
        init_add(&services, name, console_service, NULL, c, true);
    }
    size_t count = services.count;
    spin_unlock_irq(&init_lock, flags);

    kprintf("init        : pid 1, %lu services\n", count);
    kprintf("boot complete, handing the screen to the shell\n\n");

    for (size_t i = 0; i < count; i++) {
        if (!start_service(i)) {
            /* it stays down. a service that could not be started is not
             * a service that died, so the sweep below will not try
             * again -- which is right: the reason it failed is almost
             * always no memory, and retrying that four times a second
             * forever is how a machine that is merely short of memory
             * becomes a machine that is doing nothing else */
            kprintf("init: could not start %s. it stays down -- "
                    "`init start %s` to try again\n",
                    services.s[i].name, services.s[i].name);
        }
    }

    for (;;) {
        sleep_ms(INIT_TICK_MS);

        if (stop_pending) {
            take_the_machine_down();     /* which never comes back */
        }

        /* adopted children, collected.
         *
         * this is the other half of owning them. a process whose parent
         * died is reparented here the moment the parent goes, and
         * without somebody to collect it the slot it is holding stays
         * held -- thirty-two of those and the machine cannot start
         * anything at all */
        for (;;) {
            int pid = process_orphan();
            if (pid == 0) {
                break;
            }
            /* before the slot goes, since afterwards there is nothing
             * left to ask which pipes it was holding */
            pipe_release_for(pid);
            process_collect(pid, NULL);
        }

        supervise();
    }
}

bool init_boot(void) {
    /* first into the table, so it is pid 1. that is not decoration:
     * every orphan in this kernel is reparented to a number, and the
     * number has to be one somebody can rely on */
    int pid = process_create("init", 0, 0, false, pit_uptime_ms());
    if (pid != INIT_PID) {
        return false;
    }

    struct thread *t = thread_create_parked("init", init_thread, NULL);
    if (t == NULL) {
        return false;
    }
    t->pid = INIT_PID;
    t->console = 0;
    init_tid = t->id;
    process_set_thread(INIT_PID, t->id);

    sched_wake_thread(t->id);
    return true;
}

#endif /* TINYOS_HOSTED */
