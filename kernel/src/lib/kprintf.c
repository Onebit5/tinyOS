#include "lib/kprintf.h"
#include "drivers/serial.h"
#include "drivers/console.h"
#include "cpu/interrupts.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static void putc_both(char c) {
    serial_putchar(c);
    if (console_ready()) {
        console_putchar(c);
    }
}

/* print an unsigned number in the given base, right aligned to width.
 * negative numbers: the '-' goes before zero padding ("-007") but after
 * space padding ("  -7"), like real printf does */
static void print_num(uint64_t v, unsigned base, bool negative, int width, char pad) {
    static const char digits[] = "0123456789abcdef";
    char tmp[24];   /* 64-bit decimal worst case is 20 digits */
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v > 0);

    int total = n + (negative ? 1 : 0);

    if (negative && pad == '0') {
        putc_both('-');
    }
    for (int i = total; i < width; i++) {
        putc_both(pad);
    }
    if (negative && pad == ' ') {
        putc_both('-');
    }
    while (n > 0) {
        putc_both(tmp[--n]);
    }
}

void kvprintf(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putc_both(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* l, ll, z all mean "64-bit" on this arch, dont overthink it */
        bool wide = false;
        while (*fmt == 'l' || *fmt == 'z') {
            wide = true;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            putc_both((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            while (*s) {
                putc_both(*s++);
            }
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = wide ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            bool neg = v < 0;
            /* careful: -INT64_MIN doesnt exist, the unsigned negate is fine tho */
            uint64_t u = neg ? -(uint64_t)v : (uint64_t)v;
            print_num(u, 10, neg, width, pad);
            break;
        }
        case 'u':
            print_num(wide ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, false, width, pad);
            break;
        case 'x':
            print_num(wide ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, false, width, pad);
            break;
        case 'p':
            putc_both('0');
            putc_both('x');
            print_num((uint64_t)va_arg(ap, void *), 16, false, 16, '0');
            break;
        case '%':
            putc_both('%');
            break;
        case '\0':
            return; /* fmt ended on a lone '%', just bail */
        default:
            /* unknown specifier, print it raw so its at least visible */
            putc_both('%');
            putc_both(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    /* one line at a time, please. without this two threads printing at
     * once produce a lovely mess of interleaved half-words on screen.
     * yes this means a slow framebuffer scroll can cost us a timer
     * tick -- uptime drifts a hair, the alternative is unreadable */
    uint64_t flags = irq_save();

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    irq_restore(flags);
}
