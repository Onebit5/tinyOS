#include "lib/panic.h"
#include "lib/kprintf.h"
#include <stdarg.h>

void panic(const char *fmt, ...) {
    asm volatile ("cli");

    kprintf("\n\n*** KERNEL PANIC ***\n");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\n\nsystem halted. reboot whenever youre ready\n");

    for (;;) {
        asm volatile ("hlt");
    }
}
