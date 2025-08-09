#include "shell/shell.h"
#include "drivers/keyboard.h"
#include "drivers/console.h"
#include "drivers/pit.h"
#include "cpu/system.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include <stdint.h>

#define LINE_MAX 128
#define ARGV_MAX 8
#define HISTORY_MAX 16

#define COLOR_PROMPT 0x7b8ce0   /* velvet blue */
#define COLOR_TEXT   0xc8c8d0
#define COLOR_WARN   0xe6c245

struct command {
    const char *name;
    const char *help;
    void (*fn)(int argc, char **argv);
};

static const struct command commands[];    /* defined below, after the handlers */

/* ---- the personas one may summon ---------------------------------- */

struct persona {
    const char *name;
    const char *line;
    uint64_t    period_ms;
};

static const struct persona personas[] = {
    { "pixie",      "count",   700  },
    { "jack-frost", "hee-ho!", 1300 },
};
#define PERSONA_COUNT (sizeof(personas) / sizeof(personas[0]))

/* how many times a summoned persona speaks before departing. finite on
 * purpose: an immortal thread scribbling over the prompt makes the
 * shell unusable, and watching it exit shows off the reaper anyway */
#define PERSONA_LINES 8

/* ctrl+c bumps this. every persona remembers what it was when it was
 * summoned, and takes the hint when the number moves. we have no
 * signals and no way to yank a sleeping thread off the run queue, so
 * cancelling is cooperative: a persona notices next time it wakes up,
 * which can be up to one sleep period later */
static volatile uint64_t cancel_generation;

static void persona_thread(void *arg) {
    const struct persona *p = arg;
    uint64_t summoned_at = cancel_generation;

    for (int i = 1; i <= PERSONA_LINES; i++) {
        if (cancel_generation != summoned_at) {
            kprintf("[%s] recalled to the velvet room\n", p->name);
            return;
        }
        kprintf("[%s] %s %d, uptime %lums\n", p->name, p->line, i,
                pit_uptime_ms());
        sleep_ms(p->period_ms);
    }
}

/* ---- commands ------------------------------------------------------ */

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("thou may command:\n");
    for (const struct command *c = commands; c->name; c++) {
        kprintf("  %s", c->name);
        for (size_t i = strlen(c->name); i < 9; i++) {
            kprintf(" ");
        }
        kprintf("%s\n", c->help);
    }
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    }
    kprintf("\n");
}

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t total = pmm_total_bytes();
    uint64_t used  = pmm_used_bytes();
    uint64_t freeb = pmm_free_bytes();

    kprintf("physical frames\n");
    kprintf("  total  %lu MiB (%lu frames)\n", total / (1024 * 1024),
            total / PAGE_SIZE);
    kprintf("  used   %lu KiB (%lu frames)\n", used / 1024, used / PAGE_SIZE);
    kprintf("  free   %lu MiB (%lu frames)\n", freeb / (1024 * 1024),
            freeb / PAGE_SIZE);
    kprintf("kernel heap\n");
    kprintf("  total  %lu KiB claimed from the pmm\n", kheap_total_bytes() / 1024);
    kprintf("  used   %lu bytes handed out\n", kheap_used_bytes());
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t ms = pit_uptime_ms();
    uint64_t s  = ms / 1000;
    kprintf("awake for %luh %lum %lus (%lu ticks, %lums)\n",
            s / 3600, (s / 60) % 60, s % 60, pit_ticks(), ms);
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    sched_dump();
}

static void cmd_summon(int argc, char **argv) {
    if (argc < 2) {
        kprintf("summon whom? the register holds:\n");
        for (size_t i = 0; i < PERSONA_COUNT; i++) {
            kprintf("  %s\n", personas[i].name);
        }
        return;
    }

    for (size_t i = 0; i < PERSONA_COUNT; i++) {
        if (strcmp(argv[1], personas[i].name) == 0) {
            struct thread *t = thread_create(personas[i].name, persona_thread,
                                             (void *)&personas[i]);
            if (t == NULL) {
                kprintf("the summoning failed -- no memory for a new soul\n");
                return;
            }
            console_set_colors(COLOR_PROMPT, 0x101018);
            kprintf("I am thou... thou art I...\n");
            kprintf("%s has answered thy call (thread %d)\n",
                    personas[i].name, t->id);
            console_set_colors(COLOR_TEXT, 0x101018);
            return;
        }
    }
    kprintf("no persona by the name '%s' dwells here\n", argv[1]);
}

static void cmd_crash(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_colors(COLOR_WARN, 0x101018);
    kprintf("tempting fate: reading from 0xdeadbeef...\n");
    console_set_colors(COLOR_TEXT, 0x101018);

    volatile uint64_t *bad = (volatile uint64_t *)0xdeadbeef;
    uint64_t got = *bad;

    kprintf("read back %lx -- which should have been impossible\n", got);
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    reboot();
}

static const struct command commands[] = {
    { "help",   "list what thou may command",           cmd_help   },
    { "clear",  "wipe the screen clean",                cmd_clear  },
    { "echo",   "say something back",                   cmd_echo   },
    { "mem",    "frames and heap, honestly counted",    cmd_mem    },
    { "uptime", "how long since the bond was formed",   cmd_uptime },
    { "ps",     "the threads that walk this realm",     cmd_ps     },
    { "summon", "call forth a persona thread",          cmd_summon },
    { "crash",  "tempt fate with a wild pointer",       cmd_crash  },
    { "reboot", "sever the bond and begin anew",        cmd_reboot },
    { NULL, NULL, NULL },
};

/* ---- the line editor ----------------------------------------------- */

/* chop a line into argv in place. spaces become terminators, runs of
 * them collapse, and we stop early rather than overflow argv */
static int split(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;

    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0' || argc == max) {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p == ' ') {
            *p++ = '\0';
        }
    }
    return argc;
}

static void run_line(char *line) {
    char *argv[ARGV_MAX];
    int argc = split(line, argv, ARGV_MAX);

    if (argc == 0) {
        return;     /* they just pressed enter, thats allowed */
    }

    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(argv[0], c->name) == 0) {
            c->fn(argc, argv);
            return;
        }
    }
    kprintf("'%s' means nothing to me. try 'help'\n", argv[0]);
}

static void prompt(void) {
    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("velvet> ");
    console_set_colors(COLOR_TEXT, 0x101018);
}

/* ---- history -------------------------------------------------------- */

static char history[HISTORY_MAX][LINE_MAX];
static int  hist_count;     /* how many entries are real */

static void history_add(const char *line) {
    if (line[0] == '\0') {
        return;             /* dont remember the user pressing enter */
    }
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0) {
        return;             /* dont remember the same thing twice running */
    }

    if (hist_count == HISTORY_MAX) {
        /* oldest falls off the end */
        for (int i = 1; i < HISTORY_MAX; i++) {
            for (int j = 0; j < LINE_MAX; j++) {
                history[i - 1][j] = history[i][j];
            }
        }
        hist_count--;
    }

    size_t n = 0;
    while (line[n] && n < LINE_MAX - 1) {
        history[hist_count][n] = line[n];
        n++;
    }
    history[hist_count][n] = '\0';
    hist_count++;
}

/* wipe what's on screen and put something else there instead */
static void replace_line(char *line, size_t *len, const char *with) {
    while (*len > 0) {
        kprintf("\b \b");
        (*len)--;
    }

    size_t n = 0;
    while (with[n] && n < LINE_MAX - 1) {
        line[n] = with[n];
        n++;
    }
    line[n] = '\0';
    *len = n;

    if (n > 0) {
        kprintf("%s", line);
    }
}

void shell_run(void) {
    char line[LINE_MAX];

    console_set_colors(COLOR_TEXT, 0x101018);
    kprintf("type 'help' if thou art lost. ctrl+c abandons a line,\n");
    kprintf("up and down walk through what thou hast said before.\n\n");

    for (;;) {
        size_t len = 0;
        /* where we are looking in history. == hist_count means "the
         * line im typing right now", which is the bottom of the list */
        int hist_pos = hist_count;

        prompt();

        for (;;) {
            int c = keyboard_getchar_blocking();

            if (c == '\n') {
                kprintf("\n");
                break;
            }

            if (c == KEY_CTRL_C) {
                kprintf("^C\n");
                len = 0;
                cancel_generation++;    /* and call back any personas */
                break;
            }

            if (c == KEY_UP) {
                if (hist_pos > 0) {
                    hist_pos--;
                    replace_line(line, &len, history[hist_pos]);
                }
                continue;
            }

            if (c == KEY_DOWN) {
                if (hist_pos < hist_count) {
                    hist_pos++;
                    /* walking past the newest entry lands you back on
                     * an empty line, ready to type something fresh */
                    replace_line(line, &len,
                                 hist_pos == hist_count ? "" : history[hist_pos]);
                }
                continue;
            }

            if (c == '\b') {
                if (len > 0) {
                    len--;
                    /* back up, paint over it, back up again. works on
                     * the framebuffer and on a serial terminal alike */
                    kprintf("\b \b");
                }
                continue;
            }

            /* everything else non-printable (tab, arrows we dont use,
             * stray control codes) gets quietly dropped rather than
             * blitted as a garbage glyph */
            if (c < ' ' || c > '~') {
                continue;
            }

            if (len + 1 < LINE_MAX) {
                line[len++] = (char)c;
                kprintf("%c", (char)c);
            }
        }

        line[len] = '\0';
        history_add(line);
        run_line(line);
    }
}
