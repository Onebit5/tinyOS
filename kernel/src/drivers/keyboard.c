#include "drivers/keyboard.h"
#include "drivers/input.h"
#include "cpu/io.h"
#include "cpu/pic.h"
#include "cpu/interrupts.h"
#include <stdbool.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_IRQ    1

/* scancode set 1 -> ascii, us layout. 0 means "nothing printable here",
 * which covers modifiers, f-keys and everything else we dont care about.
 * 27 is escape, which will draw as a weird glyph if you print it. thats
 * between you and your conscience */

static const char keymap[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  /* lctrl */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  /* lshift */
    '\\','z','x','c','v','b','n','m',',','.','/',
    0,  /* rshift */
    '*',
    0,  /* lalt */
    ' ',
    0,  /* capslock, and nothing but zeroes past here */
};

static const char keymap_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,
    '|','Z','X','C','V','B','N','M','<','>','?',
    0,
    '*',
    0,
    ' ',
    0,
};

static bool lshift, rshift, caps;
static bool lctrl, rctrl;
static bool e0_prefix;

/* the e0-prefixed keys we care about. everything else with an e0 in
 * front still gets quietly dropped */
static void feed_extended(uint8_t code, bool release) {
    if (code == 0x1d) {         /* right ctrl is a modifier, not a key */
        rctrl = !release;
        return;
    }
    if (release) {
        return;
    }
    switch (code) {
    case 0x48: input_push(KEY_UP);     break;
    case 0x50: input_push(KEY_DOWN);   break;
    case 0x4b: input_push(KEY_LEFT);   break;
    case 0x4d: input_push(KEY_RIGHT);  break;
    case 0x53: input_push(KEY_DELETE); break;
    default: break;
    }
}

void keyboard_feed(uint8_t sc) {
    if (sc == 0xe0) {
        e0_prefix = true;
        return;
    }

    bool release = sc & 0x80;
    uint8_t code = sc & 0x7f;

    if (e0_prefix) {
        e0_prefix = false;
        feed_extended(code, release);
        return;
    }

    switch (code) {
    case 0x2a: lshift = !release; return;
    case 0x36: rshift = !release; return;
    case 0x1d: lctrl  = !release; return;
    case 0x3a:
        if (!release) {
            caps = !caps;
        }
        return;
    }

    if (release) {
        return;
    }

    char c = (lshift || rshift) ? keymap_shift[code] : keymap[code];
    if (c == 0) {
        return;
    }

    /* ctrl+letter collapses to the matching control code, so ctrl+c
     * arrives as 3 and the shell can treat it like every terminal
     * has since forever. ctrl+anything-else we simply drop */
    if (lctrl || rctrl) {
        if (c >= 'a' && c <= 'z') {
            input_push(c - 'a' + 1);
        } else if (c >= 'A' && c <= 'Z') {
            input_push(c - 'A' + 1);
        }
        return;
    }

    /* capslock flips letters only, and cancels against shift */
    if (caps) {
        if (c >= 'a' && c <= 'z') {
            c += 'A' - 'a';
        } else if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }

    input_push((uint16_t)c);
}

static void keyboard_irq(struct interrupt_frame *f) {
    (void)f;
    keyboard_feed(inb(KBD_DATA));
}

void keyboard_init(void) {
    /* drain whatever stale bytes the 8042 has been hoarding since boot */
    while (inb(KBD_STATUS) & 1) {
        inb(KBD_DATA);
    }
    irq_register(KBD_IRQ, keyboard_irq);
    pic_unmask(KBD_IRQ);
}
