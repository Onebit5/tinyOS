/* serial input translation. a terminal speaks a different dialect than
 * a ps/2 keyboard -- CR for enter, DEL for backspace, escape sequences
 * for the arrows -- and serial_feed() is what reconciles them */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

struct interrupt_frame;
void irq_register(uint8_t irq, void (*h)(struct interrupt_frame *)) { (void)irq; (void)h; }
void pic_unmask(uint8_t irq) { (void)irq; }
struct waitq;
void waitq_block(struct waitq *q) { (void)q; }
void waitq_wake_all(struct waitq *q) { (void)q; }

#include "drivers/serial.h"
#include "drivers/input.h"

static int failures = 0;

static void feed(const char *bytes) {
    for (const char *p = bytes; *p; p++) serial_feed((uint8_t)*p);
}

static void expect_keys(const int *want, int wantn, const char *what) {
    int got[64], n = 0, c;
    while ((c = input_getchar()) >= 0 && n < 64) got[n++] = c;
    int bad = (n != wantn);
    for (int i = 0; !bad && i < n; i++) bad = (got[i] != want[i]);
    if (bad) {
        printf("FAIL %s: got [", what);
        for (int i = 0; i < n; i++) printf("%#x%s", got[i], i + 1 < n ? " " : "");
        printf("] want [");
        for (int i = 0; i < wantn; i++) printf("%#x%s", want[i], i + 1 < wantn ? " " : "");
        printf("]\n");
        failures++;
    }
}

int main(void) {
    feed("ps");
    expect_keys((int[]){'p', 's'}, 2, "plain letters pass through");

    feed("\r");
    expect_keys((int[]){'\n'}, 1, "CR from a terminal becomes newline");

    feed("\n");
    expect_keys((int[]){'\n'}, 1, "LF is already a newline");

    serial_feed(0x7f);
    expect_keys((int[]){'\b'}, 1, "DEL becomes backspace");

    serial_feed(0x08);
    expect_keys((int[]){'\b'}, 1, "a real backspace stays one");

    serial_feed(0x03);
    expect_keys((int[]){0x03}, 1, "ctrl+c arrives as 3 already");

    /* arrows come in as ESC [ A and friends */
    feed("\x1b[A");
    expect_keys((int[]){KEY_UP}, 1, "ESC[A is up");

    feed("\x1b[B");
    expect_keys((int[]){KEY_DOWN}, 1, "ESC[B is down");

    feed("\x1b[C\x1b[D");
    expect_keys((int[]){KEY_RIGHT, KEY_LEFT}, 2, "ESC[C and ESC[D are right and left");

    /* a CSI sequence we dont handle must vanish, not spray garbage */
    feed("\x1b[H");
    expect_keys(NULL, 0, "an unknown escape sequence is dropped");

    /* the state machine must not swallow real input around a sequence */
    feed("a\x1b[Ab");
    expect_keys((int[]){'a', KEY_UP, 'b'}, 3, "text either side of an arrow survives");

    /* a lone ESC that isnt followed by [ shouldnt eat the next key */
    feed("\x1b" "x");
    expect_keys(NULL, 0, "bare ESC consumes only the byte after it");

    feed("ok");
    expect_keys((int[]){'o', 'k'}, 2, "and recovers cleanly afterwards");

    /* a whole command line, as the CI script types it */
    feed("mem\r");
    expect_keys((int[]){'m','e','m','\n'}, 4, "a full command line");

    if (failures == 0) printf("all good\n");
    return failures;
}
