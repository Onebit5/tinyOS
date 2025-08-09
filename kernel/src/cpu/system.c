#include "cpu/system.h"
#include "cpu/io.h"
#include "lib/kprintf.h"
#include "drivers/console.h"
#include "drivers/pit.h"
#include <stdint.h>

/* reboot via the 8042 keyboard controller reset line, the traditional way.
 * nothing calls this yet -- the shell gets a reboot command in m6, its
 * just already dressed for the occasion */

void reboot(void) {
    /* note: interrupts stay ON through the farewell, because the wait
     * below counts timer ticks and the timer cannot tick with them off.
     * we only shut the door once theres nothing left to wait for */
    console_set_colors(0x7b8ce0, 0x101018);
    kprintf("\nThou art I... And I am thou...\n");
    kprintf("Thou hast established a genuine bond...\n\n");
    kprintf("The innermost power of the Computer\n");
    kprintf("Arcana hath been set free.\n\n");
    kprintf("We bestow upon thee the ability to\n");
    kprintf("create tinyOS, the ultimate form\n");
    kprintf("of the Computer's Arcana...\n\n");

    /* let the words hang there for a moment. we have a real timer now,
     * so no more counting to a made up number and hoping */
    pit_busy_wait(3000);

    asm volatile ("cli");
    outb(0x64, 0xfe);   /* pulse the reset line */

    /* if that didnt do it, just sit here in the dark */
    for (;;) {
        asm volatile ("hlt");
    }
}
