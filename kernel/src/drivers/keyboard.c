#include "drivers/keyboard.h"
#include "cpu/io.h"
#include "cpu/pic.h"
#include "cpu/interrupts.h"

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

/* single producer (the irq), single consumer, single core. head and tail
 * only ever move forward on their own side, so no locking gymnastics */
#define KBUF_SIZE 256
static unsigned char kbuf[KBUF_SIZE];
static volatile unsigned int khead, ktail;

static bool lshift, rshift, caps;
static bool e0_prefix;

static void buf_push(char c) {
    unsigned int next = (khead + 1) % KBUF_SIZE;
    if (next == ktail) {
        return;     /* buffer full, the keystroke returns to the sea of souls */
    }
    kbuf[khead] = (unsigned char)c;
    khead = next;
}

void keyboard_feed(uint8_t sc) {
    if (sc == 0xe0) {
        /* extended prefix: arrows, right ctrl, etc. swallow the next
         * byte and pretend none of it happened (for now) */
        e0_prefix = true;
        return;
    }
    if (e0_prefix) {
        e0_prefix = false;
        return;
    }

    bool release = sc & 0x80;
    uint8_t code = sc & 0x7f;

    switch (code) {
    case 0x2a: lshift = !release; return;
    case 0x36: rshift = !release; return;
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

    /* capslock flips letters only, and cancels against shift */
    if (caps) {
        if (c >= 'a' && c <= 'z') {
            c += 'A' - 'a';
        } else if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }

    buf_push(c);
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

int keyboard_getchar(void) {
    if (ktail == khead) {
        return -1;
    }
    int c = kbuf[ktail];
    ktail = (ktail + 1) % KBUF_SIZE;
    return c;
}

bool keyboard_haskey(void) {
    return ktail != khead;
}
