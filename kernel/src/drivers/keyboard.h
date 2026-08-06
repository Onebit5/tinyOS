#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* ps/2 keyboard, scancode set 1, us layout. interrupt driven, keys land
 * in a ring buffer until someone comes asking for them */

/* keys that arent characters get values above 0xff so they cant be
 * confused with one. ctrl+letter comes through as the usual control
 * codes instead (ctrl+c is 3, same as every terminal since 1963) */
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_DELETE 0x104

#define KEY_CTRL_C 0x03

void keyboard_init(void);
int  keyboard_getchar(void);    /* next key, or -1 if the buffer is empty */
bool keyboard_haskey(void);

/* wait for a key. the calling thread sleeps on a waitq and costs
 * nothing until the irq wakes it, which is how the shell can sit at a
 * prompt all day without burning a single cycle */
int keyboard_getchar_blocking(void);

/* the scancode state machine, split from the irq handler so it can be
 * fed synthetic bytes in host-side tests. the irq handler is just
 * keyboard_feed(inb(0x60)) */
void keyboard_feed(uint8_t scancode);

#endif
