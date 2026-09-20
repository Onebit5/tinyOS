#include "arch/x86_64/machine.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/irq.h"
#include "arch/x86_64/cpu.h"
#include <stdint.h>

/* the three ways this box answers to something other than software, and
 * the end of the line.
 *
 * these used to be `system.c`, mixed in with the farewell text that gets
 * printed before a reboot. the text is not architecture and never was --
 * it is what this kernel says when it stops, and it now lives with the
 * code that decides to stop, which is init. what is left here is three
 * pokes at a motherboard. */

void cpu_stop(void) {
    irq_disable();
    for (;;) {
        cpu_idle();
    }
}

void machine_reset(void) {
    irq_disable();
    outb(0x64, 0xfe);   /* pulse the 8042 reset line, the traditional way */

    /* if that did not take, sit here in the dark. there is nothing to
     * return to -- whoever called this has already said goodbye */
    cpu_stop();
}

bool machine_poweroff(void) {
    irq_disable();

    outw(0x604,  0x2000);   /* qemu, and anything modern enough */
    outw(0xb004, 0x2000);   /* older qemu / bochs */
    outw(0x4004, 0x3400);   /* virtualbox */

    /* still here, so nobody was listening. saying so is worth more than
     * halting quietly: a machine that sits there with the fan running
     * looks broken, and "close the window" is a complete answer */
    return false;
}

bool machine_key_pressed(void) {
    /* interrupts are off and never coming back when this is asked, so
     * the keyboard driver is no help -- it is built entirely around an
     * interrupt that will not arrive. this talks to the 8042 directly */
    if (inb(0x64) & 1) {
        uint8_t scancode = inb(0x60);
        /* bit 7 set means a key came *up*, which is probably just
         * somebody releasing whatever they were holding when it all went
         * wrong. waiting for a press means waiting for a decision */
        if (!(scancode & 0x80)) {
            return true;
        }
    }

    /* and over the serial line, for anyone driving this headless */
    if (inb(0x3f8 + 5) & 1) {
        inb(0x3f8);
        return true;
    }

    return false;
}
