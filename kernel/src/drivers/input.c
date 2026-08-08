#include "drivers/input.h"
#include "cpu/interrupts.h"
#include "sched/sched.h"

/* producers are irq handlers, the consumer is the shell thread, and
 * theres one core, so interrupts-off is all the mutual exclusion this
 * needs. 16 bits wide because arrow keys dont fit in a char */
#define INPUT_BUF_SIZE 256

static uint16_t buf[INPUT_BUF_SIZE];
static volatile unsigned int head, tail;
static struct waitq waiters;

void input_push(int key) {
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
    uint64_t flags = irq_save();
    int c = buf_pop();
    irq_restore(flags);
    return c;
}

bool input_haskey(void) {
    return tail != head;
}

int input_getchar_blocking(void) {
    uint64_t flags = irq_save();

    int c;
    while ((c = buf_pop()) < 0) {
        /* nothing there. sleep with interrupts still off so an irq
         * cant slip a key past us in the gap between looking and
         * sleeping -- waitq_block hands them back on the way out */
        waitq_block(&waiters);
    }

    irq_restore(flags);
    return c;
}
