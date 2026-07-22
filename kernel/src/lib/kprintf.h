#ifndef LIB_KPRINTF_H
#define LIB_KPRINTF_H

#include <stdarg.h>
#include <stdbool.h>

/* kernel printf. output goes to serial always, and to the framebuffer
 * console once its up. supported: %c %s %d %i %u %x %p %%, length mods
 * l/ll/z (all 64-bit here anyway), zero padding + width like %08x.
 * thats it, no floats, no fanciness */

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

/* whether kprintf reaches the screen. serial and the log always get
 * everything -- this only decides what the user is made to look at.
 * boot runs with it off, so the screen shows a greeting instead of a
 * wall of driver chatter, and `dmesg` can still show you the chatter */
void kprintf_to_console(bool on);

/* whether what is being printed should also go down the serial line.
 *
 * the serial line is a second window onto whichever console is being
 * looked at rather than a console of its own -- four shells writing
 * down one wire is four conversations in one column of text, and the
 * keyboard already hands its keys to the console on the screen. set
 * once the consoles exist; before that there is one of everything and
 * the question does not arise */
void kprintf_serial_filter(bool (*fn)(void));

/* everything kprintf has ever printed, oldest first, for `dmesg` */
void klog_dump(void);

#endif
