#ifndef DRIVERS_INPUT_H
#define DRIVERS_INPUT_H

#include <stdint.h>
#include <stdbool.h>

/* where every source of typing meets. the ps/2 keyboard pushes here,
 * and so does the serial port, so the shell neither knows nor cares
 * whether you are sitting at the machine or telnetted into its soul.
 *
 * one ring buffer, one waitq, one consumer (the shell) */

/* keys that arent characters get values above 0xff so they cant be
 * confused with one. ctrl+letter comes through as the usual control
 * codes instead (ctrl+c is 3, same as every terminal since 1963) */
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_DELETE 0x104

#define KEY_CTRL_C 0x03

/* called from irq handlers. wakes whoever is waiting */
void input_push(int key);

int  input_getchar(void);           /* next key, or -1 if nothing waiting */
bool input_haskey(void);

/* wait for a key. the calling thread sleeps on a waitq and costs
 * nothing until an irq wakes it, which is how the shell can sit at a
 * prompt all day without burning a single cycle */
int  input_getchar_blocking(void);

#endif
