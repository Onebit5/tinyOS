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

/* what character is in each cell. I need this the moment the cursor is
 * allowed to sit on top of a character instead of always trailing the
 * text: to move the cursor off a cell I have to put back whatever was
 * underneath it, and the framebuffer cannot tell me that. statically
 * sized because console_init runs long before the pmm exists */
#define MAX_COLS 256
#define MAX_ROWS 128
static unsigned char cells[MAX_ROWS][MAX_COLS];

static void fill_rect(size_t x, size_t y, size_t w, size_t h, uint32_t color) {
    for (size_t dy = 0; dy < h; dy++) {
        for (size_t dx = 0; dx < w; dx++) {
            px[(y + dy) * stride + (x + dx)] = color;
        }
    }
}

static void draw_glyph(size_t col, size_t row, unsigned char c) {
    if (row < MAX_ROWS && col < MAX_COLS) {
        cells[row][col] = c;
    }
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

/* block cursor. it covers whatever character is in the cell, so taking
 * it away means redrawing that character rather than just painting
 * over it -- which is what the shadow buffer is for */
static void draw_cursor(void) {
    fill_rect(cur_col * FONT_WIDTH, cur_row * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT, fg);
}

static void erase_cursor(void) {
    unsigned char under = (cur_row < MAX_ROWS && cur_col < MAX_COLS)
                        ? cells[cur_row][cur_col] : 0;
    if (under == 0 || under == ' ') {
        fill_rect(cur_col * FONT_WIDTH, cur_row * FONT_HEIGHT,
                  FONT_WIDTH, FONT_HEIGHT, bg);
    } else {
        draw_glyph(cur_col, cur_row, under);
    }
}

static void scroll(void) {
    /* move the whole text area up one row of characters. this reads back
     * from framebuffer memory which is famously slow, a backbuffer would
     * fix it, but qemu doesnt care and neither do i (yet) */
    size_t row_px = FONT_HEIGHT * stride;               /* pixels per char row */
    memmove((void *)px, (void *)(px + row_px), (rows - 1) * row_px * 4);
    fill_rect(0, (rows - 1) * FONT_HEIGHT, cols * FONT_WIDTH, FONT_HEIGHT, bg);

    /* the shadow buffer scrolls with the pixels or it starts lying */
    memmove(&cells[0][0], &cells[1][0], (MAX_ROWS - 1) * MAX_COLS);
    memset(&cells[MAX_ROWS - 1][0], 0, MAX_COLS);
}

static void newline(void) {
    cur_col = 0;
    if (cur_row + 1 >= rows) {
        scroll();
    } else {
        cur_row++;
    }
}

void console_init(const struct ph_framebuffer *fb) {
    if (fb->bpp != 32) {
        /* qemu always gives 32bpp so im not writing three blitters.
         * stay not-ready and let serial carry the weight */
        return;
    }
    px = (volatile uint32_t *)fb->address;
    stride = fb->pitch / 4;
    pix_w = fb->width;
    pix_h = fb->height;
    cols = pix_w / FONT_WIDTH;
    rows = pix_h / FONT_HEIGHT;
    if (cols > MAX_COLS) cols = MAX_COLS;   /* a very wide screen just
                                             * gets an unused margin */
    if (rows > MAX_ROWS) rows = MAX_ROWS;
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

void console_size(size_t *out_cols, size_t *out_rows,
                  size_t *out_width, size_t *out_height) {
    if (out_cols)   *out_cols = cols;
    if (out_rows)   *out_rows = rows;
    if (out_width)  *out_width = pix_w;
    if (out_height) *out_height = pix_h;
}

void console_clear(void) {
    fill_rect(0, 0, pix_w, pix_h, bg);
    memset(cells, 0, sizeof cells);
    cur_col = 0;
    cur_row = 0;
    draw_cursor();
}

void console_move(size_t col, size_t row) {
    if (!ready) {
        return;
    }
    if (col >= cols) {
        col = cols > 0 ? cols - 1 : 0;
    }
    if (row >= rows) {
        row = rows > 0 ? rows - 1 : 0;
    }

    /* put back whatever the block was sitting on before moving, or the
     * old position keeps a solid rectangle nobody put there */
    erase_cursor();
    cur_col = col;
    cur_row = row;
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
        /* move only. every real terminal treats backspace as cursor-left
         * and leaves the character alone, so "\b \b" erases and a bare
         * "\b" is how you walk back over text you want to keep. the
         * shell's line editor depends on both */
        if (cur_col > 0) {
            cur_col--;
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
