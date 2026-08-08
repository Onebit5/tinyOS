#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <stdbool.h>

#include <stdint.h>

/* com1 uart. this is the debugging lifeline, everything gets logged here */

bool serial_init(void);
void serial_putchar(char c);
void serial_write(const char *s);

/* turn on the receive interrupt so typing into the serial console
 * drives the shell. must come after the idt and pic are up */
void serial_input_init(void);

/* one received byte -> the input queue. terminals speak a slightly
 * different dialect than a ps/2 keyboard (cr for enter, del for
 * backspace, escape sequences for arrows) so this translates.
 * split out from the irq handler so host tests can feed it bytes */
void serial_feed(uint8_t byte);

#endif
