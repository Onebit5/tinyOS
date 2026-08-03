#include "drivers/console.h"
#include "drivers/font.h"
#include "lib/string.h"

/* framebuffer console state. single global console, its a kernel not a
 * terminal multiplexer */

static volatile uint32_t *px;   /* framebuffer, indexed in pixels */
static size_t stride;           /* pixels per scanline (pitch/4) */
static size_t pix_w, pix_h;
static size_t cols, rows;       /* size in characters */
static size_t cur_col, cur_row;
static uint32_t fg = 0xc8c8d0;  /* soft grey on almost-black */
static uint32_t bg = 0x101018;
static bool ready = false;

static void fill_rect(size_t x, size_t y, size_t w, size_t h, uint32_t color) {
    for (size_t dy = 0; dy < h; dy++) {
        for (size_t dx = 0; dx < w; dx++) {
            px[(y + dy) * stride + (x + dx)] = color;
        }
    }
}

static void draw_glyph(size_t col, size_t row, unsigned char c) {
    const uint8_t *glyph = console_font[c];
    size_t ox = col * FONT_WIDTH;
    size_t oy = row * FONT_HEIGHT;
    for (size_t y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (size_t x = 0; x < FONT_WIDTH; x++) {
            /* msb is the leftmost pixel */
            px[(oy + y) * stride + ox + x] = (bits & (0x80u >> x)) ? fg : bg;
        }
    }
}

/* block cursor. the cell under the cursor is always empty (we erase it
 * before moving away), so erasing is just painting bg */
static void draw_cursor(void) {
    fill_rect(cur_col * FONT_WIDTH, cur_row * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, fg);
}

static void erase_cursor(void) {
    fill_rect(cur_col * FONT_WIDTH, cur_row * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, bg);
}

static void scroll(void) {
    /* move the whole text area up one row of characters. this reads back
     * from framebuffer memory which is famously slow, a backbuffer would
     * fix it, but qemu doesnt care and neither do i (yet) */
    size_t row_px = FONT_HEIGHT * stride;               /* pixels per char row */
    memmove((void *)px, (void *)(px + row_px), (rows - 1) * row_px * 4);
    fill_rect(0, (rows - 1) * FONT_HEIGHT, cols * FONT_WIDTH, FONT_HEIGHT, bg);
}

static void newline(void) {
    cur_col = 0;
    if (cur_row + 1 >= rows) {
        scroll();
    } else {
        cur_row++;
    }
}

void console_init(struct limine_framebuffer *fb) {
    if (fb->bpp != 32) {
        /* qemu always gives 32bpp so im not writing three blitters.
         * stay not-ready and let serial carry the weight */
        return;
    }
    px = fb->address;
    stride = fb->pitch / 4;
    pix_w = fb->width;
    pix_h = fb->height;
    cols = pix_w / FONT_WIDTH;
    rows = pix_h / FONT_HEIGHT;
    ready = true;
    console_clear();
}

bool console_ready(void) {
    return ready;
}

void console_set_colors(uint32_t new_fg, uint32_t new_bg) {
    fg = new_fg;
    bg = new_bg;
}

void console_clear(void) {
    fill_rect(0, 0, pix_w, pix_h, bg);
    cur_col = 0;
    cur_row = 0;
    draw_cursor();
}

void console_putchar(char c) {
    if (!ready) {
        return;
    }

    erase_cursor();

    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cur_col = 0;
        break;
    case '\b':
        if (cur_col > 0) {
            cur_col--;
            fill_rect(cur_col * FONT_WIDTH, cur_row * FONT_HEIGHT,
                      FONT_WIDTH, FONT_HEIGHT, bg);
        }
        break;
    case '\t':
        /* spaces up to the next multiple of 8. lazy but it also cleans
         * whatever was under those cells, which is a feature */
        do {
            console_putchar(' ');
        } while (cur_col % 8 != 0);
        erase_cursor();  /* the recursive calls redrew it */
        break;
    default:
        draw_glyph(cur_col, cur_row, (unsigned char)c);
        cur_col++;
        if (cur_col >= cols) {
            newline();
        }
        break;
    }

    draw_cursor();
}

void console_write(const char *s) {
    while (*s) {
        console_putchar(*s++);
    }
}
