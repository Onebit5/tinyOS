#include "lib/panic.h"
#include "lib/kprintf.h"
#include "drivers/console.h"
#include "cpu/system.h"
#include "cpu/io.h"
#include <stdarg.h>
#include <stdint.h>

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
    kprintf("Yet death is not the end.\n\n");
    kprintf("Press any key to return to the Velvet Room...\n");

    /* interrupts are off and never coming back, so the keyboard driver
     * is no help here -- we talk to the 8042 ourselves. poll the status
     * port for a byte, and reset on the first press we see (bit 7 set
     * means a key came *up*, which is probably just the user releasing
     * whatever they were holding when it all went wrong) */
    for (;;) {
        if (inb(0x64) & 1) {
            uint8_t sc = inb(0x60);
            if (!(sc & 0x80)) {
                system_reset();
            }
        }
        asm volatile ("pause");
    }
}
