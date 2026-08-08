#include "serial.h"
#include "drivers/input.h"
#include "cpu/io.h"
#include "cpu/pic.h"
#include "cpu/interrupts.h"

/* com1, a 16550 uart (or whatever qemu pretends is one) */
#define COM1     0x3f8
#define COM1_IRQ 4

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

/* ---- input ---------------------------------------------------------- */

/* where we are in an escape sequence: 0 = nowhere, 1 = saw ESC,
 * 2 = saw ESC[ and the next byte says which arrow */
static int esc_state;

void serial_feed(uint8_t b) {
    if (esc_state == 1) {
        esc_state = (b == '[') ? 2 : 0;
        return;
    }
    if (esc_state == 2) {
        esc_state = 0;
        switch (b) {
        case 'A': input_push(KEY_UP);    return;
        case 'B': input_push(KEY_DOWN);  return;
        case 'C': input_push(KEY_RIGHT); return;
        case 'D': input_push(KEY_LEFT);  return;
        default:  return;   /* some other CSI sequence, not ours */
        }
    }

    switch (b) {
    case 0x1b:              /* ESC: might be an arrow, wait and see */
        esc_state = 1;
        return;
    case '\r':              /* terminals send CR for enter, we want LF */
        input_push('\n');
        return;
    case 0x7f:              /* DEL is what most terminals send for backspace */
        input_push('\b');
        return;
    default:
        input_push(b);      /* ctrl codes included -- ctrl+c is already 3 */
        return;
    }
}

static void serial_irq(struct interrupt_frame *f) {
    (void)f;
    /* drain the fifo, we may have been handed several bytes at once */
    while (inb(COM1 + 5) & 1) {
        serial_feed(inb(COM1));
    }
}

void serial_input_init(void) {
    if (!serial_ok) {
        return;
    }
    outb(COM1 + 1, 0x01);   /* interrupt when a byte arrives */
    irq_register(COM1_IRQ, serial_irq);
    pic_unmask(COM1_IRQ);
}
