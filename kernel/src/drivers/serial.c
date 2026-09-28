#include "serial.h"
#include "drivers/input.h"
#include "drivers/console.h"

/* the terminal, which is not a chip.
 *
 * this file used to be a uart *and* a terminal: fifteen `outb`s to a
 * 16550 at port 0x3f8, and underneath them a state machine that turns
 * what a terminal actually sends -- escape, bracket, a letter or a
 * number and a tilde -- into the keys the rest of the kernel believes
 * in. 0.2.20 put the whole thing on checkarch's allow list, on the
 * grounds that the second half could not be separated from the first
 * without a second machine to separate it against.
 *
 * an aarch64 port answered it and was then removed again (see
 * ROADMAP.md) -- but the answer survives the port, because it was never
 * about arm: **an 8250 is not an x86 chip.** *reaching* it through a
 * port space is the x86 part. a pl011 wired to memory needs every line
 * below this comment and not one line of what used to be above it.
 *
 * so the chip is in arch/x86_64/uart.c and what is left here is the
 * part that was never about hardware: `serial_feed` takes a byte from
 * whichever uart there is and decides which key it was. */

/* ---- input ---------------------------------------------------------- */

/* where I am in an escape sequence: 0 = nowhere, 1 = saw ESC,
 * 2 = saw ESC[ and the next byte says which key, 3 = collecting the
 * digits of a `ESC [ n ~` sequence.
 *
 * terminals send special keys in two different shapes and there is no
 * getting away with knowing only one: the arrows are a single letter,
 * while page up and page down are a number followed by a tilde. home
 * and end are sent both ways depending on the terminal, so both are
 * accepted */
static int esc_state;
static int esc_number;

/* ctrl+backslash was pressed and the next byte says which console */
static bool want_console;

void serial_feed(uint8_t b) {
    if (want_console) {
        want_console = false;
        if (b >= '1' && b <= '0' + VCONSOLE_COUNT) {
            input_push(KEY_CONSOLE_1 + (b - '1'));
        }
        return;
    }
    if (esc_state == 1) {
        esc_state = (b == '[') ? 2 : 0;
        esc_number = 0;
        return;
    }
    if (esc_state == 2 || esc_state == 3) {
        if (b >= '0' && b <= '9') {
            esc_state = 3;
            if (esc_number >= 0) {
                esc_number = esc_number * 10 + (b - '0');
                if (esc_number > 99) {
                    /* nonsense, but I am still inside a sequence -- so
                     * keep swallowing until it ends rather than letting
                     * the rest of the digits out as text */
                    esc_number = -1;
                }
            }
            return;
        }
        int was = esc_state;
        esc_state = 0;

        if (was == 3) {
            if (b != '~' || esc_number < 0) {
                return;             /* some other CSI sequence, not mine */
            }
            switch (esc_number) {
            case 1: input_push(KEY_HOME);   return;
            case 3: input_push(KEY_DELETE); return;
            case 4: input_push(KEY_END);    return;
            case 5: input_push(KEY_PGUP);   return;
            case 6: input_push(KEY_PGDN);   return;
            default: return;
            }
        }

        switch (b) {
        case 'A': input_push(KEY_UP);    return;
        case 'B': input_push(KEY_DOWN);  return;
        case 'C': input_push(KEY_RIGHT); return;
        case 'D': input_push(KEY_LEFT);  return;
        case 'H': input_push(KEY_HOME);  return;
        case 'F': input_push(KEY_END);   return;
        default:  return;   /* some other CSI sequence, not mine */
        }
    }

    switch (b) {
    case 0x1c:              /* ctrl+backslash: the next digit picks a console */
        /* a serial line has no alt key and no function keys, so
         * switching needs a sequence of ordinary bytes. ctrl+\ is
         * chosen because nothing else here uses it and no shell binds
         * it -- and `chvt` does the same thing for anyone who would
         * rather type a word */
        want_console = true;
        return;
    case 0x1b:              /* ESC: might be an arrow, wait and see */
        esc_state = 1;
        return;
    case '\r':              /* terminals send CR for enter, I want LF */
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
