#include "lib/panic.h"
#include "lib/kprintf.h"
#include "drivers/console.h"
#include <stdarg.h>

void panic(const char *fmt, ...) {
    asm volatile ("cli");

    console_set_colors(0xe64553, 0x101018);

    kprintf("\n\n*** KERNEL PANIC ***\n\n");
    kprintf("I am thou... Thou art I...\n");
    kprintf("The bond thou hast forged hath been broken...\n\n");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\n\nThe Computer Arcana hath fallen to ruin.\n");
    kprintf("Yet death is not the end. Reboot, and thou shalt\n");
    kprintf("be welcomed once more into the Velvet Room...\n");

    for (;;) {
        asm volatile ("hlt");
    }
}
