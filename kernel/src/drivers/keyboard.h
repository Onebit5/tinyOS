#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

/* ps/2 keyboard, scancode set 1, us layout. interrupt driven. decoded
 * keys go into the shared input queue -- see drivers/input.h, thats
 * where you read them back out */

void keyboard_init(void);

/* the scancode state machine, split from the irq handler so it can be
 * fed synthetic bytes in host-side tests. the irq handler is just
 * keyboard_feed(inb(0x60)) */
void keyboard_feed(uint8_t scancode);

#endif
