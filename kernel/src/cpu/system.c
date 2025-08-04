#include "cpu/system.h"
#include "cpu/io.h"
#include "lib/kprintf.h"
#include "drivers/console.h"
#include <stdint.h>

/* reboot via the 8042 keyboard controller reset line, the traditional way.
 * nothing calls this yet -- the shell gets a reboot command in m6, its
 * just already dressed for the occasion */

void reboot(void) {
    asm volatile ("cli");

    console_set_colors(0x7b8ce0, 0x101018);
    kprintf("\nThou art I... And I am thou...\n");
    kprintf("Thou hast established a genuine bond...\n\n");
    kprintf("The innermost power of the Computer\n");
    kprintf("Arcana hath been set free.\n\n");
    kprintf("We bestow upon thee the ability to\n");
    kprintf("create tinyOS, the ultimate form\n");
    kprintf("of the Computer's Arcana...\n\n");

    /* let the words hang there for a moment. no timer until m5, so we
     * count to a big number like cavemen. tuned for qemu, sue me */
    for (volatile uint64_t i = 0; i < 400000000; i++) { }

    outb(0x64, 0xfe);   /* pulse the reset line */

    /* if that didnt do it, just sit here in the dark */
    for (;;) {
        asm volatile ("hlt");
    }
}
