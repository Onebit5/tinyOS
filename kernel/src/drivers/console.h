#ifndef DRIVERS_CONSOLE_H
#define DRIVERS_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "philemon.h"

/* framebuffer text consoles. 8x16 font, scrolling, block cursor.
 * handles \n \r \b \t, everything else gets blitted as a glyph.
 *
 * there are four of them now, and only one is on the screen.
 *
 * up to 0.2.16 this was one console, which meant the terminal layer was
 * one machine pretending to be one seat: one screen, one foreground
 * process, one shell. every real system has had several since the
 * eighties for the same reason -- something is running and you want to
 * do something else, and suspending it is not the same as putting it
 * somewhere.
 *
 * what makes it possible is that the console already kept a shadow of
 * what was in every cell. that was added in 0.1.x so a block cursor
 * could put back the character it was sitting on, and it turns out to
 * be the whole of what an off-screen console is: a screen nobody is
 * looking at is a shadow buffer nobody is painting. */

#define VCONSOLE_COUNT 4

void console_init(const struct ph_framebuffer *fb);
bool console_ready(void);

/* write to whichever console the caller belongs to. that is answered by
 * the hook below, because a thread can be on any of them and an
 * interrupt is on none */
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

/* ---- which of them ---------------------------------------------------
 *
 * output goes to the console its *writer* belongs to, not to whichever
 * is on the screen. a shell on console 2 printing while console 1 is
 * displayed must not scribble over console 1 -- that is the entire
 * difference between four consoles and one console with four names.
 *
 * so console.c asks. the hook is set once at boot to something that
 * looks up the current thread; before it is set, and from an interrupt
 * where there is no current thread, everything goes to whichever is on
 * the screen, which is the only answer that could be right */
void console_set_owner_hook(unsigned (*fn)(void));

unsigned console_active(void);

/* put a different one on the screen. repaints from its shadow buffer,
 * which is the only place its contents have been living */
void console_switch(unsigned n);

/* ---- scrollback ------------------------------------------------------
 *
 * each console keeps more lines than fit on the screen. `back` is how
 * many lines up from the bottom the view is; zero is the live end.
 *
 * anything *written* while scrolled up snaps the view back to the
 * bottom, which is what every terminal does and is right for the same
 * reason: you scrolled up to read something, and new output means the
 * thing you were reading is no longer what you want to be looking at */
void console_scroll_back(int lines);
size_t console_scrollback_lines(void);

#endif
