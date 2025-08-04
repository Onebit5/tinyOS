#ifndef LIB_KPRINTF_H
#define LIB_KPRINTF_H

#include <stdarg.h>

/* kernel printf. output goes to serial always, and to the framebuffer
 * console once its up. supported: %c %s %d %i %u %x %p %%, length mods
 * l/ll/z (all 64-bit here anyway), zero padding + width like %08x.
 * thats it, no floats, no fanciness */

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

#endif
