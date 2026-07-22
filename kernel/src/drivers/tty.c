#include "drivers/tty.h"
#include "drivers/input.h"
#include "cpu/interrupts.h"
#include "sched/spinlock.h"
#include "lib/kprintf.h"
#include "sched/process.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "drivers/console.h"

/* which process is at the front of the terminal */
static struct spinlock tty_lock = SPINLOCK("tty", LOCK_RANK_DEVICE);

/* one of each, per console. the machine has four seats now and the
 * only thing they share is the glass */
static int foreground[VCONSOLE_COUNT];

/* set by ctrl+z, taken by whoever was waiting. the shell has no other
 * way to hear about it: there are no signals, so this is the message */
static int stopped_group[VCONSOLE_COUNT];

unsigned tty_my_console(void) {
    struct thread *t = sched_current();
    unsigned n = (t != NULL) ? t->console : console_active();
    return (n < VCONSOLE_COUNT) ? n : 0;
}

void tty_set_foreground(int pgid) {
    unsigned n = tty_my_console();
    uint64_t flags = spin_lock_irq(&tty_lock);
    foreground[n] = pgid;
    spin_unlock_irq(&tty_lock, flags);
}

bool tty_take_stopped(int *pgid) {
    unsigned n = tty_my_console();
    uint64_t flags = spin_lock_irq(&tty_lock);
    int g = stopped_group[n];
    stopped_group[n] = 0;
    spin_unlock_irq(&tty_lock, flags);

    if (g == 0) {
        return false;
    }
    *pgid = g;
    return true;
}

/* a process may read the keyboard only if it is at the front of its own
 * console *and* that console is the one on the screen. a shell on
 * console 3 is at the front of console 3 and is still not being typed
 * at, which is the whole point of there being more than one */
bool tty_is_current(int pid) {
    unsigned n = tty_my_console();
    if (n != console_active()) {
        return false;
    }
    return process_pgid(pid) == foreground[n];
}

int tty_foreground(void) {
    return foreground[tty_my_console()];
}

/* ctrl+z: stop the whole job where it stands.
 *
 * nothing is asked of the program and nothing needs to be. every thread
 * in the group is simply marked unpickable, keeping whatever it was in
 * the middle of -- a half-typed line, a blocked pipe read -- so that
 * continuing it later is one bit rather than a recovery */
static bool stop_foreground(unsigned console, int pgid) {
    int ids[MAX_PROCESSES];
    size_t n = process_group_threads(pgid, ids, MAX_PROCESSES);
    if (n == 0) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        sched_set_stopped(ids[i], true);
    }

    uint64_t flags = spin_lock_irq(&tty_lock);
    stopped_group[console] = pgid;
    foreground[console] = TTY_SHELL;    /* it comes back to its own shell */
    spin_unlock_irq(&tty_lock, flags);

    kprintf("\n");
    return true;
}

bool tty_intercept(int key) {
    /* switching consoles is the terminal's business and nobody else's.
     * it happens before anything looks at who is in front, because the
     * whole point of the key is that it is answered by the machine
     * rather than by whatever is running */
    if (key >= KEY_CONSOLE_1 && key < KEY_CONSOLE_1 + VCONSOLE_COUNT) {
        console_switch((unsigned)(key - KEY_CONSOLE_1));
        return true;
    }
    if (key == KEY_SCROLL_UP) {
        console_scroll_back(8);
        return true;
    }
    if (key == KEY_SCROLL_DOWN) {
        console_scroll_back(-8);
        return true;
    }

    if (key != KEY_CTRL_C && key != KEY_CTRL_Z) {
        return false;
    }

    /* an interrupt arrives from the keyboard, so it is aimed at
     * whichever console is being looked at -- not at whichever console
     * the interrupted thread happened to be on */
    unsigned console = console_active();
    int pgid = foreground[console];
    if (pgid == TTY_SHELL) {
        /* the shell is at the front. it wants ctrl+c as an ordinary key
         * so its line editor can abandon the line, and ctrl+z means
         * nothing when there is nothing running to suspend */
        return key == KEY_CTRL_Z;
    }

    if (key == KEY_CTRL_Z) {
        return stop_foreground(console, pgid);
    }

    /* ctrl+c reaches every member. a pipeline is one thing to whoever
     * typed it, and interrupting only the last of three would leave the
     * other two writing into a pipe nobody is reading */
    int ids[MAX_PROCESSES];
    size_t n = process_group_threads(pgid, ids, MAX_PROCESSES);
    if (n == 0) {
        return true;
    }

    /* asking twice means insisting: the first one is delivered and the
     * program may do as it likes with it, the second stops being a
     * request. asked of the group's leader, since that is the one whose
     * answer the person is waiting on */
    if (process_interrupt_pending(pgid)) {
        kprintf("\n[tty] job %d did not take the hint\n", pgid);
        for (size_t i = 0; i < n; i++) {
            sched_kill(ids[i]);
        }
        return true;
    }

    process_interrupt_group(pgid);

    /* wake them, wherever they are. a program asleep or blocked on a
     * read has to come back and look at the flag, or an interrupt would
     * only arrive whenever it next happened to do something */
    for (size_t i = 0; i < n; i++) {
        sched_wake_thread(ids[i]);
    }
    return true;
}

int64_t tty_read_line(int pid, char *buf, uint64_t len) {
    /* only the job at the front of the console being *looked at* may
     * read. a background program helping itself would take keys from
     * whoever is being typed at -- and so would a foreground program on
     * a console nobody is watching */
    if (len == 0 || !tty_is_current(pid)) {
        return -1;
    }
    if (process_take_interrupt(pid)) {
        return -1;              /* interrupted before it even began */
    }

    uint64_t n = 0;
    for (;;) {
        int c = input_getchar_blocking();

        /* the thing that woke me may have been an interrupt rather than
         * a key. leave the cursor on a fresh line, since whatever was
         * half-typed is being abandoned */
        if (process_take_interrupt(pid)) {
            kprintf("\n");
            return -1;
        }

        if (c == '\n') {
            kprintf("\n");
            if (n < len) {
                buf[n++] = '\n';   /* a read gives you the newline too */
            }
            return (int64_t)n;
        }

        if (c == '\b') {
            if (n > 0) {
                n--;
                kprintf("\b \b");  /* off the screen as well as the buffer */
            }
            continue;
        }

        /* arrows and other non-characters have no meaning in a line
         * this simple, and printing them would draw nonsense */
        if (c < ' ' || c > '~') {
            continue;
        }

        if (n + 1 >= len) {
            /* the buffer is full. hand over what there is rather than
             * dropping keys silently or writing past the end */
            return (int64_t)n;
        }
        buf[n++] = (char)c;
        kprintf("%c", (char)c);     /* the echo, which is the whole point */
    }
}
