/* the ps/2 scancode state machine, fed synthetic set-1 bytes.
 * keyboard_feed() is deliberately split from the irq handler in the
 * driver precisely so this test can exist */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* kernel bits the driver links against but doesnt need here */
struct interrupt_frame;
void irq_register(uint8_t irq, void (*h)(struct interrupt_frame *)) { (void)irq; (void)h; }
void pic_unmask(uint8_t irq) { (void)irq; }
struct waitq;
void waitq_block(struct waitq *q) { (void)q; }
void waitq_wake_all(struct waitq *q) { (void)q; }

#include "drivers/keyboard.h"
#include "drivers/input.h"

static int failures = 0;

static void feed(const uint8_t *bytes, int n) {
    for (int i = 0; i < n; i++) keyboard_feed(bytes[i]);
}

/* drain as text, for the plain-character cases */
static void expect(const char *want, const char *what) {
    char got[512];
    int n = 0, c;
    while ((c = input_getchar()) >= 0 && n < 511) got[n++] = (char)c;
    got[n] = 0;
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* drain as raw key codes, for ctrl and the arrows */
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
    feed((uint8_t[]){0x1e, 0x9e}, 2);
    expect("a", "plain letter");

    feed((uint8_t[]){0x2a, 0x1e, 0x9e, 0xaa, 0x1e, 0x9e}, 6);
    expect("Aa", "shift letter");

    feed((uint8_t[]){0x36, 0x02, 0x82, 0xb6, 0x02, 0x82}, 6);
    expect("!1", "shift digit");

    feed((uint8_t[]){0x3a, 0xba, 0x1e, 0x9e, 0x02, 0x82}, 6);
    expect("A1", "capslock lifts letters but not digits");

    feed((uint8_t[]){0x2a, 0x1e, 0x9e, 0xaa}, 4);
    expect("a", "caps and shift cancel out");

    feed((uint8_t[]){0x3a, 0xba, 0x1e, 0x9e}, 4);
    expect("a", "caps off again");

    feed((uint8_t[]){0x2a, 0xaa, 0x38, 0xb8}, 4);
    expect("", "modifiers alone are silent");

    feed((uint8_t[]){0x1c, 0x39, 0x0e, 0x0f}, 4);
    expect("\n \b\t", "enter, space, backspace, tab");

    feed((uint8_t[]){0x23,0xa3, 0x12,0x92, 0x26,0xa6, 0x26,0xa6, 0x18,0x98}, 10);
    expect("hello", "a word, typed like a person");

    /* ---- ctrl ---- */
    feed((uint8_t[]){0x1d, 0x2e, 0xae, 0x9d}, 4);
    expect_keys((int[]){0x03}, 1, "ctrl+c is 0x03");

    feed((uint8_t[]){0x1d, 0x1e, 0x9e, 0x9d}, 4);
    expect_keys((int[]){0x01}, 1, "ctrl+a is 0x01");

    feed((uint8_t[]){0xe0, 0x1d, 0x2e, 0xae, 0xe0, 0x9d}, 6);
    expect_keys((int[]){0x03}, 1, "right ctrl works the same");

    feed((uint8_t[]){0x2e, 0xae}, 2);
    expect("c", "letters are plain again once ctrl is released");

    feed((uint8_t[]){0x1d, 0x02, 0x82, 0x9d}, 4);
    expect_keys(NULL, 0, "ctrl+digit is dropped, not mangled");

    /* ---- arrows ---- */
    feed((uint8_t[]){0xe0, 0x48}, 2);
    expect_keys((int[]){KEY_UP}, 1, "arrow up");

    feed((uint8_t[]){0xe0, 0x50}, 2);
    expect_keys((int[]){KEY_DOWN}, 1, "arrow down");

    feed((uint8_t[]){0xe0, 0x4b, 0xe0, 0x4d}, 4);
    expect_keys((int[]){KEY_LEFT, KEY_RIGHT}, 2, "arrow left then right");

    feed((uint8_t[]){0xe0, 0xc8, 0xe0, 0xd0}, 4);
    expect_keys(NULL, 0, "arrow releases are silent");

    feed((uint8_t[]){0x1e, 0x9e, 0xe0, 0x48, 0x30, 0xb0}, 6);
    expect_keys((int[]){'a', KEY_UP, 'b'}, 3, "arrows keep their place in the queue");

    feed((uint8_t[]){0xe0, 0x5b, 0xe0, 0xdb, 0x1e, 0x9e}, 6);
    expect("a", "an e0 key we dont know is swallowed whole");

    /* ---- overflow ---- */
    for (int i = 0; i < 400; i++) feed((uint8_t[]){0x1e, 0x9e}, 2);
    int n = 0;
    while (input_getchar() >= 0) n++;
    if (n != 255) {
        printf("FAIL overflow: drained %d keys, want 255\n", n);
        failures++;
    }

    if (failures == 0) printf("all good\n");
    return failures;
}
