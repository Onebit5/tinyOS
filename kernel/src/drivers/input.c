#include "drivers/input.h"
#include "cpu/interrupts.h"
#include "sched/spinlock.h"
#include "sched/sched.h"
#include "drivers/tty.h"

/* producers are irq handlers, the consumer is the shell thread, and
 * theres one core, so interrupts-off is all the mutual exclusion this
 * needs. 16 bits wide because arrow keys dont fit in a char */
#define INPUT_BUF_SIZE 256

/* the ring the keyboard and the serial port both push into, from
 * interrupt handlers, on whichever core takes the interrupt */
static struct spinlock input_lock = SPINLOCK("input", LOCK_RANK_DEVICE);

static uint16_t buf[INPUT_BUF_SIZE];
static volatile unsigned int head, tail;
static struct waitq waiters;

void input_push(int key) {
    /* the tty gets first refusal. ctrl+c aimed at a program is an
     * interrupt rather than a character, and must not end up in the
     * buffer where somebody would later read it as one */
    if (tty_intercept(key)) {
        return;
    }

    unsigned int next = (head + 1) % INPUT_BUF_SIZE;
    if (next == tail) {
        return;     /* buffer full, the keystroke returns to the sea of souls */
    }
    buf[head] = (uint16_t)key;
    head = next;

    waitq_wake_all(&waiters);
}

/* the raw pop, no locking. callers below hold interrupts down */
static int buf_pop(void) {
    if (tail == head) {
        return -1;
    }
    int c = buf[tail];
    tail = (tail + 1) % INPUT_BUF_SIZE;
    return c;
}

int input_getchar(void) {
    uint64_t flags = spin_lock_irq(&input_lock);
    int c = buf_pop();
    spin_unlock_irq(&input_lock, flags);
    return c;
}

int input_peek(void) {
    uint64_t flags = spin_lock_irq(&input_lock);
    int c = (tail == head) ? -1 : buf[tail];
    spin_unlock_irq(&input_lock, flags);
    return c;
}

bool input_haskey(void) {
    return tail != head;
}

int input_getchar_blocking(void) {
    for (;;) {
        uint64_t flags = spin_lock_irq(&input_lock);
        int c = buf_pop();
        if (c >= 0) {
            spin_unlock_irq(&input_lock, flags);
            return c;
        }

        /* nothing there. go on the queue while I still hold the lock, so
         * a key arriving the instant I let go finds me on it -- that is
         * what closes the gap between looking and sleeping */
        waitq_enqueue(&waiters);
        spin_unlock_irq(&input_lock, flags);

        /* and only now stop running. this lock must not be held across
         * the sleep: interrupts-off rides through a context switch with
         * the thread, but a lock does not, and the keyboard interrupt
         * that would wake me needs this very one */
        waitq_sleep();
    }
}
