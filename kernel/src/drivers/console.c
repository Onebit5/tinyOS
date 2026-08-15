#include "drivers/console.h"
#include "drivers/font.h"
#include "lib/string.h"

/* four consoles, one screen.
 *
 * the trick is that this file already kept a shadow of what was in
 * every cell -- added so a block cursor could put back the character it
 * was sitting on. a console nobody is looking at is exactly that shadow
 * with nothing painting it, so most of what follows is the same code it
 * always was with `if (this is the one on the screen)` in front of the
 * parts that touch pixels. */

static volatile uint32_t *px;   /* framebuffer, indexed in pixels */
static size_t stride;           /* pixels per scanline (pitch/4) */
static size_t pix_w, pix_h;
static size_t cols, rows;       /* size in characters */
static bool ready = false;

#define MAX_COLS   256
#define MAX_ROWS   128

/* how much is remembered above the top of the screen. a console is
 * MAX_ROWS of screen and this many lines of what has already gone past,
 * held in one array whose last `rows` entries are what you can see */
#define SCROLLBACK 128
#define TOTAL_ROWS (MAX_ROWS + SCROLLBACK)

/* ---- colour ----------------------------------------------------------
 *
 * a cell has to remember what colour it was drawn in, not just which
 * character it is. that did not matter while the screen was only ever
 * painted forwards: the colour in force at the time was the colour that
 * went on the glass, and nothing ever went back.
 *
 * a console being switched away from and back *is* going back. without
 * this, everything repaints in whatever colour happened to be current
 * -- so switching to console 2 and back left console 1 in one flat
 * colour, prompt and all.
 *
 * a full colour per cell would be eight bytes each and megabytes
 * overall. a palette costs one byte, and a shell uses about four
 * colours */
#define PALETTE_MAX 16

static struct { uint32_t fg, bg; } palette[PALETTE_MAX];
static unsigned palette_used = 1;       /* 0 is the default, set at init */

static unsigned pen_for(uint32_t fg, uint32_t bg) {
    for (unsigned i = 0; i < palette_used; i++) {
        if (palette[i].fg == fg && palette[i].bg == bg) {
            return i;
        }
    }
    if (palette_used == PALETTE_MAX) {
        /* more colours than I keep room for. the default is a better
         * answer than an arbitrary other one, and this cannot happen
         * with anything that currently prints */
        return 0;
    }
    palette[palette_used].fg = fg;
    palette[palette_used].bg = bg;
    return palette_used++;
}

struct vconsole {
    /* what character is in each cell, including the lines that have
     * scrolled off the top, and which pen drew it. statically sized
     * because console_init runs long before the pmm exists */
    unsigned char cells[TOTAL_ROWS][MAX_COLS];
    unsigned char attr[TOTAL_ROWS][MAX_COLS];

    size_t   cur_col, cur_row;      /* within the visible window */
    unsigned pen;                   /* what is being written in now */
    size_t   back;                  /* lines scrolled up; 0 is the bottom */
};

static struct vconsole vc[VCONSOLE_COUNT];
static unsigned active;

/* who is writing. see console.h -- output belongs to its writer's
 * console, not to whichever is on the screen */
static unsigned (*owner_hook)(void);

void console_set_owner_hook(unsigned (*fn)(void)) { owner_hook = fn; }
unsigned console_active(void) { return active; }

static struct vconsole *mine(void) {
    unsigned n = owner_hook ? owner_hook() : active;
    if (n >= VCONSOLE_COUNT) {
        n = active;
    }
    return &vc[n];
}

static bool showing(const struct vconsole *c) { return c == &vc[active]; }

/* the row of the shadow buffer that the `r`th row of the screen shows.
 * the visible window is the last `rows` entries, less however far the
 * view has been scrolled up */
static size_t shadow_row(const struct vconsole *c, size_t r) {
    return TOTAL_ROWS - rows - c->back + r;
}

/* ---- pixels ---------------------------------------------------------- */

static void fill_rect(size_t x, size_t y, size_t w, size_t h, uint32_t color) {
    for (size_t dy = 0; dy < h; dy++) {
        for (size_t dx = 0; dx < w; dx++) {
            px[(y + dy) * stride + (x + dx)] = color;
        }
    }
}

static void blit(size_t col, size_t row, unsigned char c, uint32_t f,
                 uint32_t b) {
    const uint8_t *glyph = console_font[c];
    size_t x = col * FONT_WIDTH, y = row * FONT_HEIGHT;

    for (size_t dy = 0; dy < FONT_HEIGHT; dy++) {
        uint8_t bits = glyph[dy];
        for (size_t dx = 0; dx < FONT_WIDTH; dx++) {
            px[(y + dy) * stride + (x + dx)] =
                (bits & (0x80 >> dx)) ? f : b;
        }
    }
}

/* write a character into a console, and onto the screen if that console
 * is the one being looked at */
static void put_cell(struct vconsole *c, size_t col, size_t row,
                     unsigned char ch) {
    size_t sr = shadow_row(c, row);
    if (sr < TOTAL_ROWS && col < MAX_COLS) {
        c->cells[sr][col] = ch;
        c->attr[sr][col] = (unsigned char)c->pen;
    }
    if (showing(c) && ready) {
        blit(col, row, ch, palette[c->pen].fg, palette[c->pen].bg);
    }
}

static unsigned char cell_at(const struct vconsole *c, size_t col,
                             size_t row) {
    size_t sr = shadow_row(c, row);
    if (sr >= TOTAL_ROWS || col >= MAX_COLS) {
        return 0;
    }
    return c->cells[sr][col];
}

static unsigned pen_at(const struct vconsole *c, size_t col, size_t row) {
    size_t sr = shadow_row(c, row);
    if (sr >= TOTAL_ROWS || col >= MAX_COLS) {
        return 0;
    }
    unsigned p = c->attr[sr][col];
    return (p < PALETTE_MAX) ? p : 0;
}

/* block cursor. it covers whatever character is in the cell, so taking
 * it off means putting that character back -- and the framebuffer
 * cannot say what it was */
static void draw_cursor(struct vconsole *c) {
    if (!showing(c) || !ready || c->back != 0) {
        return;     /* scrolled up: the cursor is not on screen */
    }
    fill_rect(c->cur_col * FONT_WIDTH, c->cur_row * FONT_HEIGHT,
              FONT_WIDTH, FONT_HEIGHT, palette[c->pen].fg);
}

static void erase_cursor(struct vconsole *c) {
    if (!showing(c) || !ready || c->back != 0) {
        return;
    }
    unsigned char under = cell_at(c, c->cur_col, c->cur_row);
    unsigned p = pen_at(c, c->cur_col, c->cur_row);
    if (under == 0 || under == ' ') {
        fill_rect(c->cur_col * FONT_WIDTH, c->cur_row * FONT_HEIGHT,
                  FONT_WIDTH, FONT_HEIGHT, palette[p].bg);
    } else {
        blit(c->cur_col, c->cur_row, under, palette[p].fg, palette[p].bg);
    }
}


/* ---- something to point with -----------------------------------------
 *
 * one mouse, however many consoles, so the pointer follows whichever is
 * on the screen. declared up here because the repaint below has to know
 * about the highlight, and a highlight is what a selection *is* */

static bool ptr_on;
static size_t ptr_col, ptr_row;

/* a selection is a run in reading order, from where the button went
 * down to wherever it is now. holding the two ends as linear positions
 * rather than as points is what makes "is this cell in it" one
 * comparison instead of four */
static bool sel_on;
static size_t sel_anchor, sel_end;

static size_t linear(size_t col, size_t row) { return row * cols + col; }

static bool selected(size_t col, size_t row) {
    if (!sel_on) {
        return false;
    }
    size_t at = linear(col, row);
    size_t lo = (sel_anchor <= sel_end) ? sel_anchor : sel_end;
    size_t hi = (sel_anchor <= sel_end) ? sel_end : sel_anchor;
    return at >= lo && at <= hi;
}

/* paint a whole console from its shadow. the only way an off-screen
 * console's contents ever reach the glass */
static void repaint(struct vconsole *c) {
    if (!ready) {
        return;
    }
    fill_rect(0, 0, pix_w, pix_h, palette[0].bg);
    for (size_t r = 0; r < rows; r++) {
        for (size_t col = 0; col < cols; col++) {
            unsigned char ch = cell_at(c, col, r);
            unsigned p = pen_at(c, col, r);
            bool invert = selected(col, r)
                       || (ptr_on && col == ptr_col && r == ptr_row);
            uint32_t f = invert ? palette[p].bg : palette[p].fg;
            uint32_t b = invert ? palette[p].fg : palette[p].bg;

            if (ch != 0 && ch != ' ') {
                /* in the colour it was written in, which is the whole
                 * reason a cell remembers one */
                blit(col, r, ch, f, b);
            } else if (b != palette[0].bg) {
                fill_rect(col * FONT_WIDTH, r * FONT_HEIGHT,
                          FONT_WIDTH, FONT_HEIGHT, b);
            }
        }
    }
    draw_cursor(c);
}

/* ---- moving down the page -------------------------------------------- */

static void scroll(struct vconsole *c) {
    /* the shadow always scrolls, whether anybody is looking or not --
     * that is what makes a console that nobody is watching still be a
     * console when they come back to it */
    memmove(&c->cells[0][0], &c->cells[1][0], (TOTAL_ROWS - 1) * MAX_COLS);
    memset(&c->cells[TOTAL_ROWS - 1][0], 0, MAX_COLS);
    memmove(&c->attr[0][0], &c->attr[1][0], (TOTAL_ROWS - 1) * MAX_COLS);
    memset(&c->attr[TOTAL_ROWS - 1][0], 0, MAX_COLS);

    if (showing(c) && ready) {
        /* the pixels move too, rather than being repainted, because
         * reading back from framebuffer memory is famously slow and a
         * whole-screen repaint per line would be felt */
        size_t row_px = FONT_HEIGHT * stride;
        memmove((void *)px, (void *)(px + row_px), (rows - 1) * row_px * 4);
        fill_rect(0, (rows - 1) * FONT_HEIGHT, cols * FONT_WIDTH,
                  FONT_HEIGHT, palette[0].bg);
    }
}

static void newline(struct vconsole *c) {
    c->cur_col = 0;
    if (c->cur_row + 1 >= rows) {
        scroll(c);
    } else {
        c->cur_row++;
    }
}

/* ---- the whole of it ------------------------------------------------- */

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

    /* pen zero is the default, and everything starts written in it --
     * which is what makes a zeroed console legible rather than black on
     * black */
    palette[0].fg = 0xc8c8d0;   /* soft grey on almost-black */
    palette[0].bg = 0x101018;
    palette_used = 1;

    for (unsigned i = 0; i < VCONSOLE_COUNT; i++) {
        memset(&vc[i], 0, sizeof vc[i]);
    }
    active = 0;
    ready = true;

    fill_rect(0, 0, pix_w, pix_h, palette[0].bg);
    draw_cursor(&vc[0]);
}

bool console_ready(void) { return ready; }

void console_set_colors(uint32_t new_fg, uint32_t new_bg) {
    mine()->pen = pen_for(new_fg, new_bg);
}

void console_size(size_t *out_cols, size_t *out_rows,
                  size_t *out_width, size_t *out_height) {
    if (out_cols)   *out_cols = cols;
    if (out_rows)   *out_rows = rows;
    if (out_width)  *out_width = pix_w;
    if (out_height) *out_height = pix_h;
}

void console_clear(void) {
    struct vconsole *c = mine();

    /* the scrollback goes too. `clear` means "I do not want to see any
     * of that", and leaving it one shift+pageup away would be a
     * different command */
    memset(c->cells, 0, sizeof c->cells);
    memset(c->attr, 0, sizeof c->attr);
    c->cur_col = 0;
    c->cur_row = 0;
    c->back = 0;

    if (showing(c) && ready) {
        fill_rect(0, 0, pix_w, pix_h, palette[0].bg);
        draw_cursor(c);
    }
}

void console_move(size_t col, size_t row) {
    struct vconsole *c = mine();
    if (col >= cols) {
        col = cols > 0 ? cols - 1 : 0;
    }
    if (row >= rows) {
        row = rows > 0 ? rows - 1 : 0;
    }

    erase_cursor(c);
    c->cur_col = col;
    c->cur_row = row;
    draw_cursor(c);
}

void console_putchar(char c) {
    struct vconsole *v = mine();
    if (!ready && showing(v)) {
        return;
    }

    /* anything written while scrolled up snaps the view back. you
     * scrolled up to read something, and new output means what you were
     * reading is no longer what you want to be looking at */
    if (v->back != 0) {
        v->back = 0;
        if (showing(v)) {
            repaint(v);
        }
    }

    erase_cursor(v);

    switch (c) {
    case '\n':
        newline(v);
        break;
    case '\r':
        v->cur_col = 0;
        break;
    case '\b':
        /* move only. every real terminal treats backspace as cursor-left
         * and leaves the character alone, so "\b \b" erases and a bare
         * "\b" is how you walk back over text you want to keep. the
         * shell's line editor depends on both */
        if (v->cur_col > 0) {
            v->cur_col--;
        }
        break;
    case '\t':
        /* spaces up to the next multiple of 8. lazy but it also cleans
         * whatever was under those cells, which is a feature */
        do {
            put_cell(v, v->cur_col, v->cur_row, ' ');
            v->cur_col++;
            if (v->cur_col >= cols) {
                newline(v);
            }
        } while (v->cur_col % 8 != 0);
        break;
    default:
        put_cell(v, v->cur_col, v->cur_row, (unsigned char)c);
        v->cur_col++;
        if (v->cur_col >= cols) {
            newline(v);
        }
        break;
    }

    draw_cursor(v);
}

void console_write(const char *s) {
    while (*s) {
        console_putchar(*s++);
    }
}

/* one cell, as it should look right now: its own colours, or swapped if
 * it is selected or under the pointer. swapping rather than a colour of
 * its own, because a highlight has to be visible whatever the text
 * under it was written in */
static void paint_cell(struct vconsole *c, size_t col, size_t row) {
    if (!showing(c) || !ready) {
        return;
    }
    unsigned char ch = cell_at(c, col, row);
    unsigned p = pen_at(c, col, row);

    bool invert = selected(col, row)
               || (ptr_on && col == ptr_col && row == ptr_row);

    uint32_t f = invert ? palette[p].bg : palette[p].fg;
    uint32_t b = invert ? palette[p].fg : palette[p].bg;

    if (ch == 0 || ch == ' ') {
        fill_rect(col * FONT_WIDTH, row * FONT_HEIGHT,
                  FONT_WIDTH, FONT_HEIGHT, b);
    } else {
        blit(col, row, ch, f, b);
    }
}

void console_pointer(size_t col, size_t row, uint8_t buttons,
                     uint8_t pressed, uint8_t released) {
    struct vconsole *c = &vc[active];
    if (!ready || col >= cols || row >= rows) {
        return;
    }

    size_t was_col = ptr_col, was_row = ptr_row;
    bool was_on = ptr_on;

    ptr_col = col;
    ptr_row = row;
    ptr_on = true;

    if (pressed & 0x1) {                /* left: start a selection */
        sel_on = true;
        sel_anchor = linear(col, row);
        sel_end = sel_anchor;
        repaint(c);
        return;
    }
    if ((buttons & 0x1) && sel_on) {    /* dragging: extend it */
        size_t now = linear(col, row);
        if (now != sel_end) {
            sel_end = now;
            repaint(c);
            return;
        }
    }
    (void)released;

    /* nothing but the pointer moved, so only the two cells it was in
     * and is in need repainting. dragging repaints the screen and
     * moving does not, which is the difference between a pointer that
     * feels attached to the mouse and one that does not */
    if (was_on && (was_col != col || was_row != row)) {
        bool save = ptr_on;
        ptr_on = false;
        paint_cell(c, was_col, was_row);
        ptr_on = save;
    }
    paint_cell(c, col, row);
}

bool console_pointer_visible(void) { return ptr_on; }

size_t console_selection(char *out, size_t max) {
    if (!sel_on || max == 0) {
        return 0;
    }
    struct vconsole *c = &vc[active];

    size_t lo = (sel_anchor <= sel_end) ? sel_anchor : sel_end;
    size_t hi = (sel_anchor <= sel_end) ? sel_end : sel_anchor;

    size_t n = 0;
    size_t trailing = 0;    /* spaces held back until something follows */

    for (size_t at = lo; at <= hi && n + 1 < max; at++) {
        size_t col = at % cols;
        size_t row = at / cols;
        if (row >= rows) {
            break;
        }

        if (col == 0 && at != lo) {
            /* a new line. whatever spaces were being held back were
             * padding to the edge of the screen and are dropped -- a
             * terminal pads every line, and pasting eighty spaces is
             * nobody's intention */
            trailing = 0;
            out[n++] = '\n';
        }

        unsigned char ch = cell_at(c, col, row);
        if (ch == 0) {
            ch = ' ';
        }
        if (ch == ' ') {
            trailing++;
            continue;
        }

        /* something real, so the spaces before it were real too */
        while (trailing > 0 && n + 1 < max) {
            out[n++] = ' ';
            trailing--;
        }
        trailing = 0;
        out[n++] = (char)ch;
    }

    out[n] = '\0';
    return n;
}

/* ---- switching -------------------------------------------------------- */

void console_switch(unsigned n) {
    if (n >= VCONSOLE_COUNT || n == active) {
        return;
    }
    /* the selection belonged to the console being left. carrying it
     * over would mean a highlight over text it was never made from */
    sel_on = false;
    active = n;
    repaint(&vc[active]);
}

/* ---- scrollback -------------------------------------------------------- */

void console_scroll_back(int lines) {
    struct vconsole *c = &vc[active];

    long want = (long)c->back + lines;
    if (want < 0) {
        want = 0;
    }
    if (want > SCROLLBACK) {
        want = SCROLLBACK;
    }
    if ((size_t)want == c->back) {
        return;
    }

    c->back = (size_t)want;
    repaint(c);
}

size_t console_scrollback_lines(void) { return vc[active].back; }
