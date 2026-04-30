/* margaret: a text editor.
 *
 * she keeps the compendium, which is the only thing in the velvet room
 * that is written down. it seemed like the right name for the program
 * that writes things down.
 *
 * it is nano's shape, because nano's shape is the correct one for an
 * editor you have to be able to use without having read anything first:
 * the text fills the screen, the keys are on the screen, and nothing is
 * modal. there is no vi in here and there is not going to be -- modes
 * are a fine idea and a terrible one to meet by accident.
 *
 * the whole thing is one redraw per keystroke. every key builds the
 * entire screen into a buffer and writes it in one call, rather than
 * trying to work out which cells changed. that is more bytes than it
 * needs to be and it is *completely* reliable, which at eighty by
 * twenty-five is a trade worth making without thinking about it: the
 * clever version is a second model of the screen, and a second model of
 * anything is a second thing that can be wrong. */

#include "syscall.h"
#include "args.h"

static const struct opt margaret_opts[] = {
    { 'r', "readonly", false, "look, but do not let me save" },
};

static const struct program margaret = {
    .name = "margaret",
    .usage = "margaret [file]",
    .summary = "write things down; ^G for the keys",
    .opts = margaret_opts,
    .opt_count = sizeof margaret_opts / sizeof margaret_opts[0],
};

/* how much can be held at once. a fixed table rather than a rope or a
 * gap buffer: those are the right answer for an editor that opens a
 * hundred megabytes, and this one says so and stops instead, which is
 * the honest version of the same decision */
#define MAX_LINES 600
#define MAX_COL   240
#define CUT_LINES 64

static char  text[MAX_LINES][MAX_COL];
static int   len[MAX_LINES];        /* how long each line really is */
static int   lines = 1;

static char  cut[CUT_LINES][MAX_COL];
static int   cut_len[CUT_LINES];
static int   cut_count;
static bool  cut_is_fresh;          /* the last key was also a cut */

static int   cy, cx;                /* where the cursor is, in the text */
static int   top;                   /* the first line on screen */
static bool  dirty;
static bool  readonly;
static bool  too_big;

static char  name[128];
static char  message[128];
static char  needle[64];

static uint32_t cols = 80, rows = 25;

/* ---- the little things a program without a libc has to bring ------- */

static void copy(char *dst, const char *src, long cap) {
    long i = 0;
    while (src[i] != '\0' && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void say(const char *s) { copy(message, s, sizeof message); }

static void say_num(const char *before, long n, const char *after) {
    char buf[128];
    long at = 0;
    for (const char *p = before; *p && at < 100; p++) buf[at++] = *p;

    char digits[24];
    int d = 0;
    if (n == 0) digits[d++] = '0';
    while (n > 0) { digits[d++] = (char)('0' + n % 10); n /= 10; }
    while (d-- > 0 && at < 120) buf[at++] = digits[d];

    for (const char *p = after; *p && at < 126; p++) buf[at++] = *p;
    buf[at] = '\0';
    say(buf);
}

/* ---- the text ------------------------------------------------------ */

static void blank(void) {
    lines = 1;
    len[0] = 0;
    text[0][0] = '\0';
}

static bool load(const char *path) {
    long fd = open(path);
    if (fd < 0) {
        return false;       /* a new file, which is not an error */
    }

    blank();
    char buf[512];
    long n;
    while ((n = read_fd(fd, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') {
                continue;   /* a file written elsewhere. drop them quietly */
            }
            if (c == '\n') {
                if (lines >= MAX_LINES) {
                    too_big = true;
                    close(fd);
                    return true;
                }
                len[lines] = 0;
                text[lines][0] = '\0';
                lines++;
                continue;
            }
            int l = lines - 1;
            if (len[l] < MAX_COL - 1) {
                text[l][len[l]++] = c;
                text[l][len[l]] = '\0';
            } else {
                too_big = true;
            }
        }
    }
    close(fd);

    /* a file ending in a newline gave me one empty line too many: the
     * newline ends the last line rather than starting another */
    if (lines > 1 && len[lines - 1] == 0) {
        lines--;
    }
    return true;
}

static bool save(void) {
    if (readonly) {
        say("opened read-only, so there is nothing to save");
        return false;
    }
    if (name[0] == '\0') {
        say("this has no name yet, so I do not know where to put it");
        return false;
    }

    long fd = create(name);
    if (fd < 0) {
        say("cannot write there");
        return false;
    }

    long total = 0;
    for (int i = 0; i < lines; i++) {
        if (len[i] > 0) {
            long done = 0;
            while (done < len[i]) {
                long w = write_fd(fd, text[i] + done, len[i] - done);
                if (w <= 0) {
                    close(fd);
                    say("the write stopped partway -- the disk may be full");
                    return false;
                }
                done += w;
            }
            total += len[i];
        }
        write_fd(fd, "\n", 1);
        total++;
    }
    close(fd);

    dirty = false;
    say_num("wrote ", total, " bytes");
    return true;
}

/* ---- moving about --------------------------------------------------- */

static void clamp_x(void) {
    if (cx > len[cy]) cx = len[cy];
    if (cx < 0) cx = 0;
}

static void scroll_to_cursor(void) {
    int page = (int)rows - 2;       /* one line for the title, one for keys */
    if (page < 1) page = 1;

    if (cy < top) top = cy;
    if (cy >= top + page) top = cy - page + 1;
    if (top < 0) top = 0;
}

/* ---- editing -------------------------------------------------------- */

static void insert_char(char c) {
    if (readonly) {
        say("read-only");
        return;
    }
    if (len[cy] >= MAX_COL - 1) {
        say("that line is as long as I can hold");
        return;
    }
    for (int i = len[cy]; i > cx; i--) {
        text[cy][i] = text[cy][i - 1];
    }
    text[cy][cx] = c;
    len[cy]++;
    text[cy][len[cy]] = '\0';
    cx++;
    dirty = true;
}

static void open_line(void) {
    if (readonly) { say("read-only"); return; }
    if (lines >= MAX_LINES) {
        say("that is as many lines as I can hold");
        return;
    }

    for (int i = lines; i > cy + 1; i--) {
        copy(text[i], text[i - 1], MAX_COL);
        len[i] = len[i - 1];
    }
    lines++;

    /* everything right of the cursor goes to the new line */
    int moved = len[cy] - cx;
    for (int i = 0; i < moved; i++) {
        text[cy + 1][i] = text[cy][cx + i];
    }
    text[cy + 1][moved] = '\0';
    len[cy + 1] = moved;

    len[cy] = cx;
    text[cy][cx] = '\0';

    cy++;
    cx = 0;
    dirty = true;
}

static void join_with_previous(void) {
    if (cy == 0) return;
    int prev = cy - 1;
    if (len[prev] + len[cy] >= MAX_COL - 1) {
        say("those two lines together are longer than I can hold");
        return;
    }

    cx = len[prev];
    for (int i = 0; i < len[cy]; i++) {
        text[prev][len[prev] + i] = text[cy][i];
    }
    len[prev] += len[cy];
    text[prev][len[prev]] = '\0';

    for (int i = cy; i < lines - 1; i++) {
        copy(text[i], text[i + 1], MAX_COL);
        len[i] = len[i + 1];
    }
    lines--;
    cy = prev;
    dirty = true;
}

static void backspace(void) {
    if (readonly) { say("read-only"); return; }
    if (cx > 0) {
        for (int i = cx - 1; i < len[cy] - 1; i++) {
            text[cy][i] = text[cy][i + 1];
        }
        len[cy]--;
        text[cy][len[cy]] = '\0';
        cx--;
        dirty = true;
        return;
    }
    join_with_previous();
}

static void delete_forward(void) {
    if (readonly) { say("read-only"); return; }
    if (cx < len[cy]) {
        for (int i = cx; i < len[cy] - 1; i++) {
            text[cy][i] = text[cy][i + 1];
        }
        len[cy]--;
        text[cy][len[cy]] = '\0';
        dirty = true;
        return;
    }
    /* at the end of a line, forward delete pulls the next one up --
     * which is backspace from the start of that line, exactly */
    if (cy + 1 < lines) {
        int was = cx;
        cy++;
        cx = 0;
        join_with_previous();
        cx = was;
    }
}

/* ---- cut, copy, paste ----------------------------------------------
 *
 * by whole lines. that is nano's model and it is the right one for an
 * editor with no mouse and no selection: a selection needs an anchor,
 * something to draw it with, and a rule for every key that could move
 * one end of it, and none of that earns its keep here.
 *
 * consecutive cuts pile up, so ^K^K^K takes three lines as one lump
 * rather than leaving only the last. that is the single thing that
 * makes line-based cutting usable rather than infuriating. */

static void remember(int line, bool fresh) {
    if (fresh || cut_count >= CUT_LINES) {
        cut_count = 0;
    }
    copy(cut[cut_count], text[line], MAX_COL);
    cut_len[cut_count] = len[line];
    cut_count++;
}

static void cut_line(void) {
    if (readonly) { say("read-only"); return; }

    remember(cy, !cut_is_fresh);

    if (lines == 1) {
        len[0] = 0;
        text[0][0] = '\0';
    } else {
        for (int i = cy; i < lines - 1; i++) {
            copy(text[i], text[i + 1], MAX_COL);
            len[i] = len[i + 1];
        }
        lines--;
        if (cy >= lines) cy = lines - 1;
    }
    cx = 0;
    dirty = true;
    say_num("cut ", cut_count, cut_count == 1 ? " line" : " lines");
}

static void copy_line(void) {
    remember(cy, !cut_is_fresh);
    if (cy + 1 < lines) cy++;
    cx = 0;
    say_num("copied ", cut_count, cut_count == 1 ? " line" : " lines");
}

static void paste(void) {
    if (readonly) { say("read-only"); return; }
    if (cut_count == 0) {
        say("nothing has been cut yet");
        return;
    }
    if (lines + cut_count > MAX_LINES) {
        say("no room to paste that");
        return;
    }

    for (int i = lines - 1; i >= cy; i--) {
        copy(text[i + cut_count], text[i], MAX_COL);
        len[i + cut_count] = len[i];
    }
    for (int i = 0; i < cut_count; i++) {
        copy(text[cy + i], cut[i], MAX_COL);
        len[cy + i] = cut_len[i];
    }
    lines += cut_count;
    cy += cut_count;
    if (cy >= lines) cy = lines - 1;
    cx = 0;
    dirty = true;
    say_num("pasted ", cut_count, cut_count == 1 ? " line" : " lines");
}

/* ---- asking a question in the bottom line -------------------------- */

static void draw(void);

/* read a short answer, echoed where the key list normally is. returns
 * false if they changed their mind, which ^C and escape both mean */
static bool ask(const char *question, char *into, long cap) {
    long n = 0;
    into[0] = '\0';

    for (;;) {
        char line[160];
        long at = 0;
        for (const char *p = question; *p && at < 100; p++) line[at++] = *p;
        for (long i = 0; i < n && at < 150; i++) line[at++] = into[i];
        line[at] = '\0';
        say(line);
        draw();
        cursor_to((long)(at < (long)cols - 1 ? at : (long)cols - 2),
                  (long)rows - 1);

        long k = getkey();
        if (k < 0 || k == 27) {
            message[0] = '\0';
            return false;
        }
        if (k == '\n') {
            into[n] = '\0';
            message[0] = '\0';
            return true;
        }
        if (k == '\b') {
            if (n > 0) n--;
            continue;
        }
        if (k >= ' ' && k < 0x7f && n < cap - 1) {
            into[n++] = (char)k;
        }
    }
}

static bool confirm(const char *question) {
    say(question);
    draw();
    long k = getkey();
    message[0] = '\0';
    return (k == 'y' || k == 'Y');
}

/* ---- searching ------------------------------------------------------ */

static bool at(int line, int col, const char *what) {
    int i = 0;
    while (what[i] != '\0') {
        if (col + i >= len[line] || text[line][col + i] != what[i]) {
            return false;
        }
        i++;
    }
    return true;
}

/* from just after the cursor, wrapping round to where it started. the
 * wrap matters more than it looks: a search that stops at the bottom of
 * the file makes you jump to the top by hand to be sure you have seen
 * everything, and then you are doing the editor's job for it */
static bool find_next(void) {
    if (needle[0] == '\0') {
        return false;
    }

    int line = cy;
    int col = cx + 1;

    for (int pass = 0; pass <= lines; pass++) {
        while (col <= len[line]) {
            if (at(line, col, needle)) {
                cy = line;
                cx = col;
                return true;
            }
            col++;
        }
        line = (line + 1) % lines;
        col = 0;
    }
    return false;
}

/* ---- drawing --------------------------------------------------------
 *
 * the entire screen into one buffer, then out in a single write. the
 * alternative is a call per line, and through a console that is a
 * syscall per line for no reason at all */

static char screen[64 * 1024];
static long fill;

static void put(char c) {
    if (fill < (long)sizeof screen - 1) {
        screen[fill++] = c;
    }
}

static void puts_upto(const char *s, long max) {
    for (long i = 0; s[i] != '\0' && i < max; i++) {
        put(s[i]);
    }
}

static void put_num(long v) {
    char d[24];
    int n = 0;
    if (v == 0) d[n++] = '0';
    while (v > 0) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n-- > 0) put(d[n]);
}

static void pad_to(long want) {
    long at = 0;
    for (long i = fill; i > 0 && screen[i - 1] != '\n'; i--) at++;
    while (at < want) { put(' '); at++; }
}

/* the whole screen, however many calls that takes. a write is clamped
 * at one page by the kernel, and a wide framebuffer is more than that
 * -- ignoring the return would silently lose the bottom of the screen
 * on exactly the machines with the most of it */
static void flush(void) {
    long done = 0;
    while (done < fill) {
        long n = write_fd(STDOUT, screen + done, fill - done);
        if (n <= 0) {
            return;
        }
        done += n;
    }
}

static void draw(void) {
    fill = 0;

    /* one short of the real width, on purpose. the console wraps the
     * instant the last column is written, so a line padded to exactly
     * the width would move to the next row by itself and then the
     * newline after it would move again -- every other row blank */
    long width = (long)cols - 1;
    int page = (int)rows - 2;
    if (page < 1) page = 1;

    /* the title, which is the only place the filename and whether it
     * has been changed are visible at all */
    put(' ');
    puts_upto(name[0] ? name : "(no name)", width - 24);
    if (dirty) puts_upto("  *", 3);
    if (readonly) puts_upto("  [read-only]", 13);
    pad_to(width - 16);
    puts_upto("ln ", 3);
    put_num(cy + 1);
    put('/');
    put_num(lines);
    put(' ');
    put('c');
    put_num(cx + 1);
    pad_to(width);
    put('\n');

    for (int r = 0; r < page; r++) {
        int i = top + r;
        if (i < lines) {
            puts_upto(text[i], width);
        }
        pad_to(width);
        if (r < page - 1) {
            put('\n');
        }
    }

    /* the bottom line is either something I have to say or the keys.
     * the keys being on the screen is most of why nano is usable
     * without having read anything first */
    put('\n');
    if (message[0] != '\0') {
        puts_upto(message, width);
    } else if (too_big) {
        puts_upto("this file is larger than I can hold -- saving would lose "
                  "the rest", width);
    } else {
        puts_upto("^G keys  ^O save  ^X leave  ^K cut  ^Y copy  ^U paste  "
                  "^W find  ^N next", width);
    }
    pad_to(width);

    clear_screen();
    flush();
}

static void keys_screen(void) {
    say("");
    fill = 0;
    puts_upto(
        "margaret -- the keys\n"
        "\n"
        "  arrows, home, end      move about\n"
        "  page up, page down     a screen at a time\n"
        "  ^A / ^E                start and end of the line\n"
        "  ^T                     go to a line by number\n"
        "\n"
        "  ^K                     cut this line. again to take the next\n"
        "                         one with it, and so on\n"
        "  ^Y                     copy this line, the same way\n"
        "  ^U                     paste what was cut or copied\n"
        "\n"
        "  ^W                     find something\n"
        "  ^N                     find it again\n"
        "\n"
        "  ^O                     save\n"
        "  ^X                     leave, asking first if anything changed\n"
        "\n"
        "  press any key\n", (long)sizeof screen);

    clear_screen();
    flush();
    (void)getkey();
}

/* ---- the loop -------------------------------------------------------- */

void _start(int argc, char **argv) {
    struct args a;
    const char *error;
    if (!args_parse(&margaret, argc, argv, &a, &error)) {
        write("margaret: ");
        write(error);
        write("\n");
        exit(1);
    }
    if (a.wants_help) {
        args_usage(&margaret);
        exit(0);
    }

    readonly = args_has(&a, &margaret, 'r');
    blank();

    if (a.count > 0) {
        copy(name, a.rest[0], sizeof name);
        if (!load(name)) {
            say("a new file");
        }
    } else {
        say("a new file -- ^O will ask where to put it");
    }

    screen_size(&cols, &rows);
    if (cols > MAX_COL) cols = MAX_COL;

    for (;;) {
        clamp_x();
        scroll_to_cursor();
        draw();

        /* the cursor goes where the *text* cursor is, one row down for
         * the title bar. this is the only reason SYS_CURSOR exists: a
         * program that paints a screen has to say where in it to look */
        long screen_row = 1 + (cy - top);
        cursor_to(cx < (int)cols - 1 ? cx : (int)cols - 2, screen_row);

        long k = getkey();
        if (k < 0) {
            /* ctrl+c. an editor that threw the file away on a stray
             * keystroke would be worse than no editor */
            say("ctrl+c does nothing here -- ^X to leave");
            continue;
        }

        bool was_cut = (k == 0x0b || k == 0x19);    /* ^K, ^Y */
        message[0] = '\0';

        switch (k) {
        case KEY_LEFT:
            if (cx > 0) {
                cx--;
            } else if (cy > 0) {
                cy--;
                cx = len[cy];
            }
            break;
        case KEY_RIGHT:
            if (cx < len[cy]) {
                cx++;
            } else if (cy + 1 < lines) {
                cy++;
                cx = 0;
            }
            break;
        case KEY_UP:    if (cy > 0) cy--; break;
        case KEY_DOWN:  if (cy + 1 < lines) cy++; break;
        case KEY_HOME:  cx = 0; break;
        case KEY_END:   cx = len[cy]; break;
        case 0x01:      cx = 0; break;              /* ^A */
        case 0x05:      cx = len[cy]; break;        /* ^E */

        case KEY_PGUP:
            cy -= (int)rows - 2;
            if (cy < 0) cy = 0;
            break;
        case KEY_PGDN:
            cy += (int)rows - 2;
            if (cy >= lines) cy = lines - 1;
            break;

        case KEY_DELETE:
        case 0x04:                                  /* ^D */
            delete_forward();
            break;
        case '\b':
            backspace();
            break;
        case '\n':
            open_line();
            break;
        case '\t':
            /* spaces rather than a tab. a tab is a promise about how
             * something else will display it, and I would rather not
             * make promises on anybody's behalf */
            for (int i = 0; i < 4; i++) insert_char(' ');
            break;

        case 0x0b:      cut_line(); break;          /* ^K */
        case 0x19:      copy_line(); break;         /* ^Y */
        case 0x15:      paste(); break;             /* ^U */

        case 0x17: {                                /* ^W */
            char want[64];
            if (ask("find: ", want, sizeof want) && want[0] != '\0') {
                copy(needle, want, sizeof needle);
                if (!find_next()) {
                    say("not found");
                }
            }
            break;
        }
        case 0x0e:                                  /* ^N */
            if (needle[0] == '\0') {
                say("nothing to find again -- ^W first");
            } else if (!find_next()) {
                say("not found");
            }
            break;

        case 0x14: {                                /* ^T */
            char where[16];
            if (ask("go to line: ", where, sizeof where)) {
                long n = 0;
                bool ok = (where[0] != '\0');
                for (const char *p = where; *p; p++) {
                    if (*p < '0' || *p > '9') { ok = false; break; }
                    n = n * 10 + (*p - '0');
                }
                if (!ok || n < 1) {
                    say("that is not a line number");
                } else {
                    cy = (n > lines) ? lines - 1 : (int)n - 1;
                    cx = 0;
                }
            }
            break;
        }

        case 0x0f:                                  /* ^O */
            if (name[0] == '\0') {
                char where[128];
                if (!ask("save as: ", where, sizeof where) || where[0] == '\0') {
                    say("not saved");
                    break;
                }
                copy(name, where, sizeof name);
            }
            save();
            break;

        case 0x07:                                  /* ^G */
            keys_screen();
            break;

        case 0x18:                                  /* ^X */
            if (dirty && !confirm("save first? (y/n, any other key to stay) ")) {
                /* they said no to saving. that is not the same as
                 * saying no to leaving, so ask that separately rather
                 * than guessing which they meant */
                if (!confirm("leave without saving? (y/n) ")) {
                    break;
                }
            } else if (dirty) {
                if (!save()) {
                    break;      /* it did not save, so do not leave */
                }
            }
            clear_screen();
            cursor_to(0, 0);
            exit(0);
            break;

        default:
            if (k >= ' ' && k < 0x7f) {
                insert_char((char)k);
            }
            break;
        }

        cut_is_fresh = was_cut;
    }
}
