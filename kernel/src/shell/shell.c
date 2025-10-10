#include "shell/shell.h"
#include "drivers/input.h"
#include "drivers/console.h"
#include "drivers/pit.h"
#include "cpu/system.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "lib/backtrace.h"
#include "drivers/rtc.h"
#include "cpu/cpuinfo.h"
#include "fs/ramdisk.h"
#include "sched/usermode.h"
#include "lib/ksyms.h"
#include "version.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include <stdint.h>
#include <stdbool.h>

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
    bool takes_file;    /* tab should offer ramdisk names after it */
};

static const struct command commands[];    /* defined below, after the handlers */
static void run_argv(int argc, char **argv);
static size_t common_prefix(const char *a, const char *b);

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

/* hex, with or without the 0x. returns false if its not a number */
static bool parse_hex(const char *s, uint64_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    if (*s == '\0') {
        return false;
    }
    uint64_t v = 0;
    for (; *s; s++) {
        uint64_t d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return false;
        v = v * 16 + d;
    }
    *out = v;
    return true;
}

static void cmd_vmm(int argc, char **argv) {
    if (argc >= 2) {
        uint64_t addr;
        if (!parse_hex(argv[1], &addr)) {
            kprintf("'%s' is not a hex address\n", argv[1]);
            return;
        }
        vmm_dump(addr);
        return;
    }

    /* no argument: show the shape of the address space by pointing at
     * one thing of each kind. the permission column is the interesting
     * part -- code is r-x, everything else is rw- */
    kprintf("pml4 at %p\n", (void *)vmm_kernel_pml4());

    kprintf("code (this very function):\n");
    vmm_dump((uint64_t)(uintptr_t)cmd_vmm);

    kprintf("a string constant:\n");
    vmm_dump((uint64_t)(uintptr_t)"velvet");

    void *heap = kmalloc(64);
    if (heap != NULL) {
        kprintf("the heap:\n");
        vmm_dump((uint64_t)(uintptr_t)heap);
        kfree(heap);
    }

    uint64_t rsp;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp));
    kprintf("this thread's stack:\n");
    vmm_dump(rsp);

    kprintf("and somewhere nobody lives:\n");
    vmm_dump(0x0000dead00000000ull);
}

/* recurse until the stack runs out. lands on the guard page the vmm
 * left unmapped below every thread stack, which turns what would be
 * silent corruption of the next thread's stack into a clean fault.
 *
 * yes gcc, we know its infinite recursion. thats the entire feature */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static uint64_t eat_stack(uint64_t depth) {
    volatile uint64_t padding[64];
    for (int i = 0; i < 64; i++) {
        padding[i] = depth;
    }
    return padding[0] + eat_stack(depth + 1);
}
#pragma GCC diagnostic pop

static void cmd_stackoverflow(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_colors(COLOR_WARN, 0x101018);
    kprintf("running off the end of this thread's stack on purpose...\n");
    console_set_colors(COLOR_TEXT, 0x101018);
    kprintf("returned %lu, which should have been impossible\n", eat_stack(0));
}

static void cmd_bt(int argc, char **argv) {
    (void)argc; (void)argv;
    /* the same walker a panic uses, just with nothing on fire */
    kbacktrace(0, 0);
}

/* ---- the ramdisk ---------------------------------------------------- */

static void cmd_ls(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!ramdisk_present()) {
        kprintf("no ramdisk was handed to us at boot\n");
        return;
    }
    struct ramdisk_file f;
    uint64_t total = 0;
    size_t files = 0;

    for (size_t i = 0; ramdisk_stat(i, &f); i++) {
        /* print the path exactly as `cat` and `run` will accept it.
         * tar stores "./bin/hello" and showing that verbatim tells you
         * to type something that then does not work */
        const char *name = f.name;
        if (name[0] == '.' && name[1] == '/') {
            name += 2;
        }

        /* the archive holds directory entries too. they have no bytes
         * and nothing to open, so they are not worth listing */
        size_t n = strlen(name);
        if (n == 0 || name[n - 1] == '/') {
            continue;
        }

        kprintf("  %6lu  %s\n", f.size, name);
        total += f.size;
        files++;
    }
    kprintf("  %lu bytes in %zu files\n", total, files);
}

/* the archive is a flat list of paths and we do not search it, so
 * `hello` will not find `bin/hello`. rather than add a path search --
 * which is magic that surprises you later -- say what they probably
 * meant, if exactly one file ends that way */
static const char *suggest_path(const char *name) {
    struct ramdisk_file f;
    const char *found = NULL;
    int matches = 0;

    for (size_t i = 0; ramdisk_stat(i, &f); i++) {
        const char *p = f.name;
        if (p[0] == '.' && p[1] == '/') {
            p += 2;
        }
        /* the part after the last slash */
        const char *base = p;
        for (const char *q = p; *q; q++) {
            if (*q == '/') {
                base = q + 1;
            }
        }
        if (*base != '\0' && strcmp(base, name) == 0) {
            found = p;
            matches++;
        }
    }
    return matches == 1 ? found : NULL;
}

static void missing(const char *what, const char *name) {
    const char *did = suggest_path(name);
    if (did != NULL) {
        kprintf("%s: no such file '%s'. didst thou mean '%s'?\n",
                what, name, did);
    } else {
        kprintf("%s: no such file '%s'. `ls` shows what there is, "
                "and tab completes it\n", what, name);
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        kprintf("cat <file> [file...] -- see `ls`\n");
        return;
    }
    for (int a = 1; a < argc; a++) {
        struct ramdisk_file f;
        if (!ramdisk_open(argv[a], &f)) {
            missing("cat", argv[a]);
            continue;
        }
        /* straight out of the archive, no copy, no allocation */
        const char *p = f.data;
        for (uint64_t i = 0; i < f.size; i++) {
            kprintf("%c", p[i]);
        }
        if (f.size > 0 && p[f.size - 1] != '\n') {
            kprintf("\n");
        }
    }
}

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        kprintf("run <program> [&] -- try `run bin/hello`\n");
        return;
    }
    /* a trailing & puts it in the background, so you can have two
     * programs at once and watch them not interfere */
    bool background = (argc >= 3 && strcmp(argv[2], "&") == 0);

    const char *why = NULL;
    if (!user_run(argv[1], background, &why)) {
        if (why == USER_RUN_NO_SUCH_FILE) {
            missing("run", argv[1]);
        } else {
            kprintf("cannot run %s: %s\n", argv[1], why);
        }
    }
}

static void cmd_dmesg(int argc, char **argv) {
    (void)argc; (void)argv;
    /* everything the boot said while the screen was being kept quiet */
    klog_dump();
}

/* ---- who and what we are -------------------------------------------- */

static void cmd_arcana(int argc, char **argv) {
    (void)argc; (void)argv;

    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("\nThou art I... And I am thou...\n\n");
    console_set_colors(COLOR_TEXT, 0x101018);

    kprintf("  THE COMPUTER ARCANA\n");
    kprintf("  rank %s -- the bond deepens with every commit\n\n", VERSION);

    kprintf("  version    tinyOS %s\n", VERSION);
    kprintf("  forged     %s, %s\n", __DATE__, __TIME__);
    kprintf("  by         gcc %s\n", __VERSION__);
    kprintf("  known      %lu functions by name\n", ksym_count);
    if (ramdisk_present()) {
        kprintf("  carrying   %zu files in the ramdisk\n", ramdisk_count());
    }
    kprintf("\n");
}

/* fastfetch, if fastfetch had read a tarot deck */
static void cmd_persona(int argc, char **argv) {
    (void)argc; (void)argv;

    static const char *mask[] = {
        "     .-\"\"\"\"\"-.     ",
        "   .'  _     _  '.   ",
        "  /   (o)   (o)   \\  ",
        " |       ---       | ",
        " |    \\  ___  /    | ",
        "  \\    '.___.'    /  ",
        "   '.           .'   ",
        "     '-._____.-'     ",
    };

    char brand[49];
    cpu_brand(brand);

    size_t cols = 0, rows = 0, w = 0, h = 0;
    console_size(&cols, &rows, &w, &h);

    uint64_t ms = pit_uptime_ms();
    uint64_t total = pmm_total_bytes() / (1024 * 1024);
    uint64_t used  = pmm_used_bytes() / (1024 * 1024);

    /* the info column, one line per line of the mask */
    const int LINES = 8;
    for (int i = 0; i < LINES; i++) {
        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("%s", mask[i]);
        console_set_colors(COLOR_TEXT, 0x101018);

        switch (i) {
        case 0:
            kprintf("velvet@tinyOS");
            break;
        case 1:
            kprintf("-------------");
            break;
        case 2:
            kprintf("arcana    the Computer, rank %s", VERSION);
            break;
        case 3:
            kprintf("persona   %s", brand);
            break;
        case 4:
            kprintf("awakened  %luh %lum %lus",
                    ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60);
            break;
        case 5:
            kprintf("souls     %zu threads bound to the wheel",
                    sched_thread_count());
            break;
        case 6:
            kprintf("memory    %lu / %lu MiB", used, total);
            break;
        case 7:
            kprintf("vision    %zux%zu (%zux%zu of glyphs)", w, h, cols, rows);
            break;
        }
        kprintf("\n");
    }
    kprintf("\n");
}

static void cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *months[] = { "", "january", "february", "march",
        "april", "may", "june", "july", "august", "september", "october",
        "november", "december" };

    struct rtc_time t;
    rtc_read(&t);
    kprintf("%02u:%02u:%02u on the %u%s of %s, %u\n",
            t.hour, t.minute, t.second, t.day,
            (t.day / 10 == 1) ? "th"
              : (t.day % 10 == 1) ? "st"
              : (t.day % 10 == 2) ? "nd"
              : (t.day % 10 == 3) ? "rd" : "th",
            months[t.month <= 12 ? t.month : 0], t.year);
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) {
        kprintf("hexdump <hex address> [bytes]\n");
        return;
    }
    uint64_t addr, count = 64;
    if (!parse_hex(argv[1], &addr)) {
        kprintf("'%s' is not a hex address\n", argv[1]);
        return;
    }
    if (argc >= 3 && !parse_hex(argv[2], &count)) {
        kprintf("'%s' is not a hex length\n", argv[2]);
        return;
    }
    if (count > 1024) {
        count = 1024;       /* you did not mean that */
    }

    for (uint64_t off = 0; off < count; off += 16) {
        uint64_t base = addr + off;

        /* ask the page tables before touching anything. a hexdump that
         * page faults on a typo would be a poor debugging tool */
        if (vmm_translate(vmm_kernel_pml4(), base) == VMM_NO_MAPPING) {
            kprintf("%p  <not mapped>\n", (void *)base);
            continue;
        }

        const unsigned char *p = (const unsigned char *)base;
        kprintf("%p ", (void *)base);
        for (int i = 0; i < 16; i++) {
            kprintf(" %02x", p[i]);
        }
        kprintf("  ");
        for (int i = 0; i < 16; i++) {
            kprintf("%c", (p[i] >= ' ' && p[i] <= '~') ? p[i] : '.');
        }
        kprintf("\n");
    }
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) {
        kprintf("kill <thread id> -- see `ps`\n");
        return;
    }
    uint64_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("'%s' is not a thread id\n", argv[1]);
            return;
        }
        id = id * 10 + (uint64_t)(*p - '0');
    }
    switch (sched_kill((int)id)) {
    case SCHED_KILL_OK:
        kprintf("thread %lu returns to the sea of souls\n", id);
        break;
    case SCHED_KILL_NO_SUCH:
        kprintf("no thread %lu walks this realm\n", id);
        break;
    case SCHED_KILL_SELF:
        kprintf("i will not unmake myself while thou art still speaking\n");
        break;
    case SCHED_KILL_PROTECTED:
        kprintf("that one keeps the wheel turning. leave it be\n");
        break;
    }
}

static void cmd_history(int argc, char **argv);   /* needs the history array */

static void cmd_time(int argc, char **argv) {
    if (argc < 2) {
        kprintf("time <command> -- how long it takes\n");
        return;
    }
    uint64_t start = pit_uptime_ms();
    run_argv(argc - 1, argv + 1);
    kprintf("[%lums]\n", pit_uptime_ms() - start);
}

static void cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    system_poweroff();
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
    { "help",   "list what thou may command",           cmd_help, false },
    { "clear",  "wipe the screen clean",                cmd_clear, false },
    { "echo",   "say something back",                   cmd_echo, false },
    { "ls",     "what the ramdisk carries",             cmd_ls, false },
    { "cat",    "read a file aloud",                    cmd_cat, true },
    { "run",    "give a program the outer ring; & for background", cmd_run, true },
    { "dmesg",  "everything boot said while you werent looking", cmd_dmesg, false },
    { "arcana", "the rank of this bond, and its making", cmd_arcana, false },
    { "persona","the face this machine wears",          cmd_persona, false },
    { "mem",    "frames and heap, honestly counted",    cmd_mem, false },
    { "uptime", "how long since the bond was formed",   cmd_uptime, false },
    { "ps",     "the threads that walk this realm",     cmd_ps, false },
    { "summon", "call forth a persona thread (in the background)", cmd_summon, false },
    { "vmm",    "what the page tables say about an address", cmd_vmm, false },
    { "bt",     "who called whom to get here",          cmd_bt, false },
    { "date",   "what the battery-backed clock believes", cmd_date, false },
    { "hexdump","look at memory, safely",                cmd_hexdump, false },
    { "kill",   "end a thread by id",                    cmd_kill, false },
    { "history","what thou hast said before",            cmd_history, false },
    { "time",   "how long a command takes",              cmd_time, false },
    { "crash",  "tempt fate with a wild pointer",       cmd_crash, false },
    { "smash",  "run off the end of the stack on purpose", cmd_stackoverflow, false },
    { "reboot", "sever the bond and begin anew",        cmd_reboot, false },
    { "poweroff","let the velvet room fade",             cmd_poweroff, false },
    { NULL, NULL, NULL, false },
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

/* dispatch an already-split command. separate from run_line so `time`
 * can hand us its own argv without re-parsing anything */
static void run_argv(int argc, char **argv) {
    if (argc == 0) {
        return;
    }
    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(argv[0], c->name) == 0) {
            c->fn(argc, argv);
            return;
        }
    }
    /* before giving up, see if they nearly typed something real. we
     * only compare leading characters -- enough to catch a fumbled
     * ending like `dmseg`, and honest about not being spell check */
    const struct command *near = NULL;
    size_t best = 0;
    int ties = 0;
    for (const struct command *c = commands; c->name; c++) {
        size_t n = common_prefix(c->name, argv[0]);
        if (n > best) {
            best = n;
            near = c;
            ties = 1;
        } else if (n == best && best > 0) {
            ties++;
        }
    }

    if (best >= 2 && ties == 1) {
        kprintf("'%s' means nothing to me. didst thou mean '%s'?\n",
                argv[0], near->name);
    } else {
        kprintf("'%s' means nothing to me. try 'help'\n", argv[0]);
    }
}

static void run_line(char *line) {
    char *argv[ARGV_MAX];
    int argc = split(line, argv, ARGV_MAX);
    run_argv(argc, argv);   /* argc 0 just means they pressed enter */
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

static void cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < hist_count; i++) {
        kprintf("  %2d  %s\n", i + 1, history[i]);
    }
}

/* ---- the visible cursor --------------------------------------------
 * the console treats \b as pure cursor-left now, same as any terminal,
 * so these work identically on the framebuffer and down the wire */

static void move_left(size_t n) {
    while (n-- > 0) {
        kprintf("\b");
    }
}

/* reprint everything from pos onward, plus a space to cover a character
 * that just shifted off the end, then come back to where we were */
static void redraw_tail(const char *line, size_t len, size_t pos) {
    for (size_t i = pos; i < len; i++) {
        kprintf("%c", line[i]);
    }
    kprintf(" ");
    move_left(len - pos + 1);
}

/* throw away what is on screen and put something else there. used by
 * the history keys, which replace the whole line at once */
static void replace_line(char *line, size_t *len, size_t *pos, const char *with) {
    move_left(*pos);                    /* back to the start of the line */
    size_t old = *len;

    size_t n = 0;
    while (with[n] && n < LINE_MAX - 1) {
        line[n] = with[n];
        kprintf("%c", with[n]);
        n++;
    }
    line[n] = '\0';

    for (size_t i = n; i < old; i++) {  /* cover whatever was longer */
        kprintf(" ");
    }
    move_left(old > n ? old - n : 0);

    *len = n;
    *pos = n;
}

static bool is_word_char(char c) {
    return c != ' ';
}

/* ---- tab completion ------------------------------------------------ */

/* how many leading characters two strings share */
static size_t common_prefix(const char *a, const char *b) {
    size_t n = 0;
    while (a[n] != '\0' && a[n] == b[n]) {
        n++;
    }
    return n;
}

/* the word the cursor is sitting in, and whether it is the first one.
 * returns where that word starts */
static size_t word_start(const char *line, size_t pos, bool *first_word) {
    size_t start = pos;
    while (start > 0 && line[start - 1] != ' ') {
        start--;
    }
    *first_word = true;
    for (size_t i = 0; i < start; i++) {
        if (line[i] != ' ') {
            *first_word = false;
            break;
        }
    }
    return start;
}

/* replace the word under the cursor with `with`, redrawing what follows */
static void replace_word(char *line, size_t *len, size_t *pos,
                         size_t start, const char *with) {
    size_t old_word = *pos - start;
    size_t tail_len = *len - *pos;
    size_t new_word = strlen(with);

    if (start + new_word + tail_len + 1 >= LINE_MAX) {
        return;
    }

    /* shuffle whatever came after the word out of the way */
    for (size_t i = 0; i < tail_len; i++) {
        line[start + new_word + i] = line[*pos + i];
    }
    for (size_t i = 0; i < new_word; i++) {
        line[start + i] = with[i];
    }
    *len = start + new_word + tail_len;
    line[*len] = '\0';

    move_left(old_word);
    for (size_t i = start; i < *len; i++) {
        kprintf("%c", line[i]);
    }
    /* blank anything the shorter word left behind */
    size_t was = start + old_word + tail_len;
    for (size_t i = *len; i < was; i++) {
        kprintf(" ");
    }
    *pos = start + new_word;
    move_left((*len > was ? *len : was) - *pos);
}

/* which command the line begins with, or NULL if it is not one we know.
 * the line must already be terminated -- complete() sees to that */
static const struct command *command_for_line(const char *line) {
    size_t i = 0;
    while (line[i] == ' ') {
        i++;
    }
    size_t start = i;
    while (line[i] != '\0' && line[i] != ' ') {
        i++;
    }
    size_t n = i - start;

    for (const struct command *c = commands; c->name; c++) {
        if (strlen(c->name) == n && memcmp(c->name, line + start, n) == 0) {
            return c;
        }
    }
    return NULL;
}

/* candidates come from one of two places depending on where you are:
 * command names in the first word, ramdisk filenames after a command
 * that takes one */
struct candidates {
    const char *items[32];
    int count;
};

static void gather(struct candidates *c, const char *line, size_t start,
                   size_t pos, bool first_word) {
    c->count = 0;
    size_t plen = pos - start;
    const char *prefix = line + start;

    if (first_word) {
        for (const struct command *cmd = commands; cmd->name; cmd++) {
            if (strlen(cmd->name) >= plen
                && common_prefix(cmd->name, prefix) >= plen
                && c->count < 32) {
                c->items[c->count++] = cmd->name;
            }
        }
        return;
    }

    /* only offer filenames after a command that actually takes one --
     * completing `echo mo<tab>` into a filename would be surprising */
    const struct command *cmd = command_for_line(line);
    if (cmd == NULL || !cmd->takes_file) {
        return;
    }

    struct ramdisk_file f;
    for (size_t i = 0; ramdisk_stat(i, &f) && c->count < 32; i++) {
        const char *name = f.name;
        if (name[0] == '.' && name[1] == '/') {
            name += 2;
        }
        size_t n = strlen(name);
        if (n == 0 || name[n - 1] == '/') {
            continue;       /* directories arent worth offering */
        }
        if (n >= plen && common_prefix(name, prefix) >= plen) {
            c->items[c->count++] = name;
        }
    }
}

static void complete(char *line, size_t *len, size_t *pos) {
    /* the editor does not keep the line terminated while you are typing
     * -- it only does that on enter -- and everything below wants a
     * string. terminate it here, where len is known */
    line[*len] = '\0';

    bool first_word;
    size_t start = word_start(line, *pos, &first_word);

    /* a bare tab in the command position does nothing on purpose:
     * dumping the whole command list is what `help` is for. after a
     * command that takes a filename there is no such list to consult,
     * so an empty word there is worth answering */
    if (*pos == start && first_word) {
        return;
    }

    struct candidates c;
    gather(&c, line, start, *pos, first_word);

    if (c.count == 0) {
        return;
    }
    if (c.count == 1) {
        replace_word(line, len, pos, start, c.items[0]);
        return;
    }

    /* several: fill in as far as they all agree, and only if that adds
     * nothing do we show the list */
    size_t shared = strlen(c.items[0]);
    for (int i = 1; i < c.count; i++) {
        size_t n = common_prefix(c.items[0], c.items[i]);
        if (n < shared) {
            shared = n;
        }
    }
    if (shared > *pos - start) {
        char partial[LINE_MAX];
        for (size_t i = 0; i < shared; i++) {
            partial[i] = c.items[0][i];
        }
        partial[shared] = '\0';
        replace_word(line, len, pos, start, partial);
        return;
    }

    kprintf("\n");
    for (int i = 0; i < c.count; i++) {
        kprintf("  %s", c.items[i]);
    }
    kprintf("\n");
    prompt();
    for (size_t i = 0; i < *len; i++) {
        kprintf("%c", line[i]);
    }
    move_left(*len - *pos);
}

void shell_run(void) {
    char line[LINE_MAX];

    console_set_colors(COLOR_TEXT, 0x101018);

    for (;;) {
        size_t len = 0;     /* characters in the line */
        size_t pos = 0;     /* where the cursor sits within them */
        int hist_pos = hist_count;

        prompt();

        for (;;) {
            int c = input_getchar_blocking();

            if (c == '\n') {
                /* print the tail we were sitting in front of, so the
                 * finished line reads properly before we move on */
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                kprintf("\n");
                break;
            }

            if (c == KEY_CTRL_C) {
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                kprintf("^C\n");
                len = 0;
                cancel_generation++;    /* and call back any personas */
                break;
            }

            /* ---- moving ---- */
            if (c == KEY_LEFT || c == 0x02) {           /* ctrl+b */
                if (pos > 0) { move_left(1); pos--; }
                continue;
            }
            if (c == KEY_RIGHT || c == 0x06) {          /* ctrl+f */
                if (pos < len) { kprintf("%c", line[pos]); pos++; }
                continue;
            }
            if (c == 0x01) {                            /* ctrl+a, home */
                move_left(pos);
                pos = 0;
                continue;
            }
            if (c == 0x05) {                            /* ctrl+e, end */
                while (pos < len) { kprintf("%c", line[pos]); pos++; }
                continue;
            }

            /* ---- history ---- */
            if (c == KEY_UP) {
                if (hist_pos > 0) {
                    hist_pos--;
                    replace_line(line, &len, &pos, history[hist_pos]);
                }
                continue;
            }
            if (c == KEY_DOWN) {
                if (hist_pos < hist_count) {
                    hist_pos++;
                    replace_line(line, &len, &pos,
                                 hist_pos == hist_count ? "" : history[hist_pos]);
                }
                continue;
            }

            /* ---- deleting ---- */
            if (c == '\b') {                            /* backspace */
                if (pos > 0) {
                    for (size_t i = pos - 1; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--; pos--;
                    move_left(1);
                    redraw_tail(line, len, pos);
                }
                continue;
            }
            if (c == KEY_DELETE || c == 0x04) {         /* del, ctrl+d */
                if (pos < len) {
                    for (size_t i = pos; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--;
                    redraw_tail(line, len, pos);
                }
                continue;
            }
            if (c == 0x15) {                            /* ctrl+u, kill line */
                replace_line(line, &len, &pos, "");
                continue;
            }
            if (c == 0x0b) {                            /* ctrl+k, kill to end */
                for (size_t i = pos; i < len; i++) { kprintf(" "); }
                move_left(len - pos);
                len = pos;
                line[len] = '\0';
                continue;
            }
            if (c == 0x17) {                            /* ctrl+w, kill a word */
                size_t start = pos;
                while (start > 0 && !is_word_char(line[start - 1])) start--;
                while (start > 0 && is_word_char(line[start - 1]))  start--;
                size_t removed = pos - start;
                if (removed > 0) {
                    for (size_t i = start; i + removed < len; i++) {
                        line[i] = line[i + removed];
                    }
                    len -= removed;
                    pos = start;
                    move_left(removed);
                    for (size_t i = pos; i < len; i++) kprintf("%c", line[i]);
                    for (size_t i = 0; i < removed; i++) kprintf(" ");
                    move_left(len - pos + removed);
                }
                continue;
            }

            if (c == 0x0c) {                            /* ctrl+l */
                /* wipe the screen and put the prompt back with whatever
                 * was half-typed, cursor where it was. same as every
                 * terminal, and it does not disturb the line */
                console_clear();
                prompt();
                for (size_t i = 0; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                move_left(len - pos);
                continue;
            }

            if (c == '\t') {
                complete(line, &len, &pos);
                continue;
            }

            /* ---- typing ---- */
            if (c < ' ' || c > '~') {
                continue;       /* anything else non-printable is not ours */
            }
            if (len + 1 < LINE_MAX) {
                for (size_t i = len; i > pos; i--) {
                    line[i] = line[i - 1];
                }
                line[pos] = (char)c;
                len++;
                /* print from here to the end, then step back to just
                 * after the character we inserted */
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                pos++;
                move_left(len - pos);
            }
        }

        line[len] = '\0';
        history_add(line);
        run_line(line);
    }
}
