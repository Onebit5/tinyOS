#include "drivers/tty.h"
#include "drivers/input.h"
#include "cpu/interrupts.h"
#include "lib/kprintf.h"
#include "sched/process.h"
#include "sched/sched.h"

static int foreground = TTY_SHELL;

void tty_set_foreground(int pid) {
    uint64_t flags = irq_save();
    foreground = pid;
    irq_restore(flags);
}

int tty_foreground(void) {
    return foreground;
}

bool tty_intercept(int key) {
    if (key != KEY_CTRL_C) {
        return false;
    }

    int pid = foreground;
    if (pid == TTY_SHELL) {
        /* the shell is at the front, and it wants ctrl+c as an ordinary
         * key so its line editor can abandon the line. nothing to do */
        return false;
    }

    /* aimed at a program. asking twice means insisting: the first one
     * is delivered and the program may do as it likes with it, the
     * second stops being a request */
    if (process_interrupt_pending(pid)) {
        const struct process *p = process_find(pid);
        int tid = (p != NULL) ? p->thread_id : 0;
        kprintf("\n[tty] pid %d did not take the hint\n", pid);
        if (tid != 0) {
            sched_kill(tid);
        }
        return true;
    }

    process_interrupt(pid);

    /* wake it, wherever it is. a program asleep or blocked on a read
     * has to come back and look at the flag, or an interrupt would
     * only arrive whenever it next happened to do something */
    const struct process *p = process_find(pid);
    if (p != NULL && p->thread_id != 0) {
        sched_wake_thread(p->thread_id);
    }
    return true;
}

int64_t tty_read_line(int pid, char *buf, uint64_t len) {
    /* only the process at the front of the terminal may read it. a
     * background program helping itself would take keys from whoever
     * is actually being typed at */
    if (pid != foreground || len == 0) {
        return -1;
    }
    if (process_take_interrupt(pid)) {
        return -1;              /* interrupted before it even began */
    }

    uint64_t n = 0;
    for (;;) {
        int c = input_getchar_blocking();

        /* the thing that woke us may have been an interrupt rather than
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
