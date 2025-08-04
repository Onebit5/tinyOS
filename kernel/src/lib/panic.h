#ifndef LIB_PANIC_H
#define LIB_PANIC_H

/* game over. prints to serial + console (if up) and parks the cpu with
 * interrupts off. takes printf formatting bc panics deserve context */

void panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));

#endif
