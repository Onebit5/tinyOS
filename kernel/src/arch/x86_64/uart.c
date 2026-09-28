#include "drivers/serial.h"
#include "drivers/input.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/interrupts.h"

/* com1, a 16550 uart (or whatever qemu pretends is one).
 *
 * split out of drivers/serial.c in 0.2.21, and the split is the point:
 * everything here is `outb` and a port number, and everything left
 * behind is a terminal escape sequence. the chip is not the x86 part --
 * an 8250 wired to memory rather than to a port space would need this
 * file rewritten and none of the other one. */

#define COM1     0x3f8
#define COM1_IRQ 4

static bool serial_ok = false;

bool serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no uart interrupts, I poll like cavemen for now */
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


/* ---- and the interrupt ---------------------------------------------- */

static void serial_irq(struct interrupt_frame *f) {

    (void)f;
    /* drain the fifo, I may have been handed several bytes at once */
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
