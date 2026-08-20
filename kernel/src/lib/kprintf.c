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
static void print_num(uint64_t v, unsigned base, bool negative, int width,
                      char pad, bool left) {
    static const char digits[] = "0123456789abcdef";
    char tmp[24];   /* 64-bit decimal worst case is 20 digits */
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v > 0);

    int total = n + (negative ? 1 : 0);

    if (left) {
        if (negative) {
            putc_both('-');
        }
        while (n > 0) {
            putc_both(tmp[--n]);
        }
        for (int i = total; i < width; i++) {
            putc_both(' ');     /* padding on the right is always spaces */
        }
        return;
    }

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

static void print_str(const char *s, int width, bool left) {
    if (s == NULL) {
        s = "(null)";
    }
    int len = 0;
    for (const char *p = s; *p; p++) {
        len++;
    }
    if (!left) {
        for (int i = len; i < width; i++) putc_both(' ');
    }
    while (*s) {
        putc_both(*s++);
    }
    if (left) {
        for (int i = len; i < width; i++) putc_both(' ');
    }
}

void kvprintf(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putc_both(*fmt);
            continue;
        }
        fmt++;

        /* flags, in any order. '-' means pad on the right instead of
         * the left, and it beats '0' -- you cannot zero-pad the right
         * hand side of a number and have it still mean the same number */
        bool left = false;
        char pad = ' ';
        for (;;) {
            if (*fmt == '-')      { left = true; fmt++; }
            else if (*fmt == '0') { pad = '0';   fmt++; }
            else break;
        }
        if (left) {
            pad = ' ';
        }

        int width = 0;
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
        case 'c': {
            char c = (char)va_arg(ap, int);
            char one[2] = { c, '\0' };
            print_str(one, width, left);
            break;
        }
        case 's':
            print_str(va_arg(ap, const char *), width, left);
            break;
        case 'd':
        case 'i': {
            int64_t v = wide ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            bool neg = v < 0;
            /* careful: -INT64_MIN doesnt exist, the unsigned negate is fine tho */
            uint64_t u = neg ? -(uint64_t)v : (uint64_t)v;
            print_num(u, 10, neg, width, pad, left);
            break;
        }
        case 'u':
            print_num(wide ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10,
                      false, width, pad, left);
            break;
        case 'x':
            print_num(wide ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16,
                      false, width, pad, left);
            break;
        case 'p':
            putc_both('0');
            putc_both('x');
            print_num((uint64_t)va_arg(ap, void *), 16, false, 16, '0', false);
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
