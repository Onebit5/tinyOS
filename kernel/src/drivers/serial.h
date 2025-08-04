#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <stdbool.h>

/* com1 uart. this is the debugging lifeline, everything gets logged here */

bool serial_init(void);
void serial_putchar(char c);
void serial_write(const char *s);

#endif
