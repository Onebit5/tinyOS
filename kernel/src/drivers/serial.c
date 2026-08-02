#include "serial.h"
#include "cpu/io.h"

/* com1, a 16550 uart (or whatever qemu pretends is one) */
#define COM1 0x3f8

static bool serial_ok = false;

bool serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no uart interrupts, we poll like cavemen for now */
    outb(COM1 + 3, 0x80);   /* dlab on so the divisor registers are visible */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8n1, dlab back off */
    outb(COM1 + 2, 0xc7);   /* enable + clear fifos */
    outb(COM1 + 4, 0x1e);   /* loopback mode, to check the chip actually works */

    outb(COM1 + 0, 0xae);   /* random test byte */
    if (inb(COM1 + 0) != 0xae) {
        return false;       /* dead or missing uart. sad but not fatal */
    }

    outb(COM1 + 4, 0x0f);   /* normal operation, rts/dtr set */
    serial_ok = true;
    return true;
}

static bool transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putchar(char c) {
    if (!serial_ok) {
        return;
    }
    if (c == '\n') {
        serial_putchar('\r');   /* terminals want crlf */
    }
    while (!transmit_empty()) {
        /* spin. its fine, its 115200 baud */
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        serial_putchar(*s++);
    }
}
