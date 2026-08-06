#include "drivers/keyboard.h"
#include "cpu/io.h"
#include "cpu/pic.h"
#include "cpu/interrupts.h"
#include "sched/sched.h"

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
 * only ever move forward on their own side, so no locking gymnastics.
 * 16 bits wide because arrow keys dont fit in a char */
#define KBUF_SIZE 256
static uint16_t kbuf[KBUF_SIZE];
static volatile unsigned int khead, ktail;

static bool lshift, rshift, caps;
static bool lctrl, rctrl;
static bool e0_prefix;

static void buf_push(uint16_t key) {
    unsigned int next = (khead + 1) % KBUF_SIZE;
    if (next == ktail) {
        return;     /* buffer full, the keystroke returns to the sea of souls */
    }
    kbuf[khead] = key;
    khead = next;
}

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
    case 0x48: buf_push(KEY_UP);     break;
    case 0x50: buf_push(KEY_DOWN);   break;
    case 0x4b: buf_push(KEY_LEFT);   break;
    case 0x4d: buf_push(KEY_RIGHT);  break;
    case 0x53: buf_push(KEY_DELETE); break;
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
            buf_push(c - 'a' + 1);
        } else if (c >= 'A' && c <= 'Z') {
            buf_push(c - 'A' + 1);
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

    buf_push((uint16_t)c);
}

/* whoever is waiting for a keystroke. only the shell ever is, but the
 * queue costs one pointer so it may as well be general */
static struct waitq kbd_waiters;

static void keyboard_irq(struct interrupt_frame *f) {
    (void)f;
    keyboard_feed(inb(KBD_DATA));
    /* wake unconditionally, even for a shift press that produced no
     * character. a spurious wakeup just means the sleeper looks at an
     * empty buffer and goes back to sleep, which is cheap and much
     * safer than trying to be clever about it */
    waitq_wake_all(&kbd_waiters);
}

void keyboard_init(void) {
    /* drain whatever stale bytes the 8042 has been hoarding since boot */
    while (inb(KBD_STATUS) & 1) {
        inb(KBD_DATA);
    }
    irq_register(KBD_IRQ, keyboard_irq);
    pic_unmask(KBD_IRQ);
}

/* the raw pop, no locking. callers below hold interrupts down */
static int buf_pop(void) {
    if (ktail == khead) {
        return -1;
    }
    int c = kbuf[ktail];
    ktail = (ktail + 1) % KBUF_SIZE;
    return c;
}

int keyboard_getchar(void) {
    uint64_t flags = irq_save();
    int c = buf_pop();
    irq_restore(flags);
    return c;
}

int keyboard_getchar_blocking(void) {
    uint64_t flags = irq_save();

    int c;
    while ((c = buf_pop()) < 0) {
        /* nothing there. sleep with interrupts still off so the irq
         * cant slip a key past us in the gap between looking and
         * sleeping -- waitq_block hands them back on the way out */
        waitq_block(&kbd_waiters);
    }

    irq_restore(flags);
    return c;
}

bool keyboard_haskey(void) {
    return ktail != khead;
}
