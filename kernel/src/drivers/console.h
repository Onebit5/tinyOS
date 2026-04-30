#ifndef DRIVERS_CONSOLE_H
#define DRIVERS_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "philemon.h"

/* framebuffer text console. 8x16 font, scrolling, block cursor.
 * handles \n \r \b \t, everything else gets blitted as a glyph */

void console_init(const struct ph_framebuffer *fb);
bool console_ready(void);
void console_putchar(char c);
void console_write(const char *s);
void console_clear(void);
void console_set_colors(uint32_t fg, uint32_t bg);

/* how big the screen is, in characters and in pixels. any pointer may
 * be NULL if you dont care about that one */
void console_size(size_t *cols, size_t *rows, size_t *width, size_t *height);

/* put the cursor at a given cell, without writing anything.
 *
 * nothing wanted this until there was a program drawing whole screens.
 * a shell only ever moves the cursor by printing, and the editor cannot
 * -- it paints twenty-four lines and then has to say where in them the
 * cursor belongs */
void console_move(size_t col, size_t row);

#endif
