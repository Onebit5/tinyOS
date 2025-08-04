#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* ps/2 keyboard, scancode set 1, us layout. interrupt driven, keys land
 * in a ring buffer until someone comes asking for them */

void keyboard_init(void);
int  keyboard_getchar(void);    /* next char, or -1 if the buffer is empty */
bool keyboard_haskey(void);

/* the scancode state machine, split from the irq handler so it can be
 * fed synthetic bytes in host-side tests. the irq handler is just
 * keyboard_feed(inb(0x60)) */
void keyboard_feed(uint8_t scancode);

#endif
