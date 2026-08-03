#ifndef DRIVERS_CONSOLE_H
#define DRIVERS_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include "limine.h"

/* framebuffer text console. 8x16 font, scrolling, block cursor.
 * handles \n \r \b \t, everything else gets blitted as a glyph */

void console_init(struct limine_framebuffer *fb);
bool console_ready(void);
void console_putchar(char c);
void console_write(const char *s);
void console_clear(void);
void console_set_colors(uint32_t fg, uint32_t bg);

#endif
