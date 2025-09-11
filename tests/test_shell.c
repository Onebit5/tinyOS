/* host-side test for the shell's line splitting and command dispatch.
 * includes shell.c directly so the static helpers are reachable, and
 * stubs out every piece of kernel it leans on. kprintf is captured so
 * we can assert on exactly what the shell would have printed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- captured output ---- */
static char out[4096];
static size_t out_len;

static void out_reset(void) { out[0] = 0; out_len = 0; }

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    out_len += vsnprintf(out + out_len, sizeof(out) - out_len, fmt, ap);
    va_end(ap);
}

/* ---- kernel stubs ---- */
struct limine_framebuffer;
void console_clear(void) { kprintf("<CLEAR>"); }
void console_set_colors(uint32_t f, uint32_t b) { (void)f; (void)b; }
bool console_ready(void) { return true; }
int input_getchar_blocking(void) { return '\n'; }
uint64_t pit_uptime_ms(void) { return 12345; }
uint64_t pit_ticks(void) { return 1234; }
void reboot(void) { kprintf("<REBOOT>"); exit(0); }
uint64_t pmm_total_bytes(void) { return 2046ull * 1024 * 1024; }
uint64_t pmm_used_bytes(void) { return 100ull * 1024; }
uint64_t pmm_free_bytes(void) { return 2045ull * 1024 * 1024; }
uint64_t kheap_total_bytes(void) { return 36 * 1024; }
uint64_t kheap_used_bytes(void) { return 512; }
void sched_dump(void) { kprintf("<PS>"); }
void klog_dump(void) { kprintf("<DMESG>"); }
static bool run_ok = true;
static const char *ran_path;
#include "sched/usermode.h"
const char *const USER_RUN_NO_SUCH_FILE = "no such file in the ramdisk";
static const char *run_error = "not an elf";
bool user_run(const char *path, const char **error) {
    ran_path = path;
    if (run_ok) return true;
    *error = run_error;
    return false;
}
void vmm_dump(uint64_t v) { kprintf("<VMM %#lx>", v); }
void kbacktrace(uint64_t rbp, uint64_t rip) { (void)rbp; (void)rip; kprintf("<BT>"); }
void system_poweroff(void) { kprintf("<POWEROFF>"); exit(0); }
uint64_t vmm_translate(uint64_t pml4, uint64_t v) { (void)pml4; (void)v; return v; }
#include "drivers/rtc.h"
void rtc_read(struct rtc_time *t) {
    t->second = 5; t->minute = 4; t->hour = 3;
    t->day = 2; t->month = 1; t->year = 2026;
}
void cpu_brand(char *buf) { strcpy(buf, "Imaginary CPU @ 1 Hz"); }
void console_size(size_t *c, size_t *r, size_t *w, size_t *h) {
    if (c) *c = 160; if (r) *r = 50; if (w) *w = 1280; if (h) *h = 800;
}
const unsigned long ksym_count = 442;

/* the real ramdisk parser, mounted on the real archive the build
 * produces. the stubs that used to live here handed out tidy names
 * like "motd.txt", while tar actually stores "./motd.txt" -- so the
 * tests agreed with themselves and disagreed with the kernel */
#include "fs/ramdisk.h"

#include "sched/sched.h"
size_t sched_thread_count(void) { return 4; }
static enum sched_kill_result kill_answer = SCHED_KILL_OK;
static int killed_id = -1;
enum sched_kill_result sched_kill(int id) { killed_id = id; return kill_answer; }
uint64_t vmm_kernel_pml4(void) { return 0x1000; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void sleep_ms(uint64_t ms) { (void)ms; }

/* cmd_summon reads t->id off whatever we hand back, so hand back
 * something real rather than a poked-in pointer value */
#include "sched/thread.h"
static struct thread spawned = { .id = 42 };
static int created;
static const char *created_name;
struct thread *thread_create(const char *n, void (*e)(void *), void *a) {
    (void)e; (void)a;
    created++; created_name = n;
    return &spawned;
}

#include "shell/shell.c"

/* ---- tests ---- */
static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static void check_split(const char *input, int want_argc, const char *want0,
                        const char *want1) {
    char buf[128];
    char *argv[ARGV_MAX];
    snprintf(buf, sizeof buf, "%s", input);
    int argc = split(buf, argv, ARGV_MAX);
    if (argc != want_argc) {
        printf("FAIL split(\"%s\"): argc=%d want %d\n", input, argc, want_argc);
        failures++;
        return;
    }
    if (want0 && (argc < 1 || strcmp(argv[0], want0) != 0)) {
        printf("FAIL split(\"%s\"): argv[0]=\"%s\" want \"%s\"\n",
               input, argc > 0 ? argv[0] : "(none)", want0);
        failures++;
    }
    if (want1 && (argc < 2 || strcmp(argv[1], want1) != 0)) {
        printf("FAIL split(\"%s\"): argv[1]=\"%s\" want \"%s\"\n",
               input, argc > 1 ? argv[1] : "(none)", want1);
        failures++;
    }
}

static void run(const char *line) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", line);
    out_reset();
    run_line(buf);
}

int main(void) {
    /* mount the archive the build just made, so completion is exercised
     * against the names the kernel really sees */
    {
        FILE *fp = fopen("bin/ramdisk.tar", "rb");
        if (fp == NULL) {
            printf("FAIL: no bin/ramdisk.tar -- run `make bin/ramdisk.tar`\n");
            return 1;
        }
        static uint8_t tarbytes[1024 * 1024];
        size_t n = fread(tarbytes, 1, sizeof tarbytes, fp);
        fclose(fp);
        ramdisk_mount(tarbytes, n);
        if (!ramdisk_present()) {
            printf("FAIL: the archive did not parse\n");
            return 1;
        }
    }

    /* ---- splitting ---- */
    check_split("help", 1, "help", NULL);
    check_split("echo hello", 2, "echo", "hello");
    check_split("   echo   hello   ", 2, "echo", "hello");
    check_split("", 0, NULL, NULL);
    check_split("      ", 0, NULL, NULL);
    check_split("summon jack-frost", 2, "summon", "jack-frost");
    /* more words than ARGV_MAX must clamp, not scribble past the array */
    check_split("a b c d e f g h i j k l", ARGV_MAX, "a", "b");

    /* ---- dispatch ---- */
    run("help");
    CHECK(strstr(out, "summon") && strstr(out, "reboot"),
          "help lists the commands");

    run("echo thou art I");
    CHECK(strcmp(out, "thou art I\n") == 0, "echo rejoins its arguments");

    run("echo");
    CHECK(strcmp(out, "\n") == 0, "bare echo prints just a newline");

    run("");
    CHECK(out_len == 0, "empty line does nothing at all");

    run("     ");
    CHECK(out_len == 0, "whitespace-only line does nothing");

    run("clear");
    CHECK(strcmp(out, "<CLEAR>") == 0, "clear reaches the console");

    run("ps");
    CHECK(strcmp(out, "<PS>") == 0, "ps reaches the scheduler");

    run("uptime");
    CHECK(strstr(out, "12345") != NULL, "uptime reports the real number");

    run("mem");
    CHECK(strstr(out, "2046") && strstr(out, "36"),
          "mem reports both pmm and heap");

    run("bt");
    CHECK(strcmp(out, "<BT>") == 0, "bt reaches the stack walker");

    run("nonsense");
    CHECK(strstr(out, "nonsense") && strstr(out, "help"),
          "unknown command names itself and points at help");

    /* summon: known, unknown, and bare */
    created = 0;
    run("summon pixie");
    CHECK(created == 1 && strcmp(created_name, "pixie") == 0,
          "summon pixie spawns a thread named pixie");

    created = 0;
    run("summon gorgon");
    CHECK(created == 0 && strstr(out, "gorgon") != NULL,
          "unknown persona spawns nothing and says so");

    created = 0;
    run("summon");
    CHECK(created == 0 && strstr(out, "pixie") && strstr(out, "jack-frost"),
          "bare summon lists the register");

    /* ---- history ---- */
    hist_count = 0;
    history_add("mem");
    history_add("ps");
    CHECK(hist_count == 2, "two commands remembered");
    CHECK(strcmp(history[0], "mem") == 0 && strcmp(history[1], "ps") == 0,
          "history is in the order they were typed");

    history_add("ps");
    CHECK(hist_count == 2, "the same command twice running is remembered once");

    history_add("");
    CHECK(hist_count == 2, "a bare enter is not remembered");

    history_add("mem");
    CHECK(hist_count == 3, "a repeat that isnt adjacent still counts");

    /* overflow: fill past HISTORY_MAX and check the oldest fall off */
    hist_count = 0;
    char tmp[32];
    for (int i = 0; i < HISTORY_MAX + 5; i++) {
        snprintf(tmp, sizeof tmp, "cmd%d", i);
        history_add(tmp);
    }
    CHECK(hist_count == HISTORY_MAX, "history caps at HISTORY_MAX");
    CHECK(strcmp(history[HISTORY_MAX - 1], "cmd20") == 0,
          "newest command is at the bottom");
    CHECK(strcmp(history[0], "cmd5") == 0,
          "oldest survivor is the right one after rotation");

    /* a line longer than LINE_MAX must be truncated, not overflow */
    hist_count = 0;
    char big[LINE_MAX * 2];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    history_add(big);
    CHECK(strlen(history[0]) == LINE_MAX - 1, "overlong line truncated safely");

    /* ---- replace_line: what the history keys do to the screen ---- */
    {
        char line[LINE_MAX] = "hello";
        size_t len = 5, pos = 5;
        out_reset();
        replace_line(line, &len, &pos, "ps");
        CHECK(strcmp(line, "ps") == 0 && len == 2 && pos == 2,
              "replace_line swaps the content and leaves the cursor at the end");
        /* walk back over the old text, print the new, blank the excess,
         * then step back over the blanks */
        CHECK(strcmp(out, "\b\b\b\b\b" "ps" "   " "\b\b\b") == 0,
              "replace_line covers the longer line it replaced");

        out_reset();
        replace_line(line, &len, &pos, "");
        CHECK(line[0] == 0 && len == 0 && pos == 0,
              "replace_line can clear the line entirely");

        len = 0; pos = 0; line[0] = 0;
        out_reset();
        replace_line(line, &len, &pos, "uptime");
        CHECK(strcmp(out, "uptime") == 0 && len == 6 && pos == 6,
              "growing from an empty line just prints");
    }

    /* ---- cursor helpers ---- */
    {
        out_reset();
        move_left(3);
        CHECK(strcmp(out, "\b\b\b") == 0, "move_left is pure backspaces");

        char line[LINE_MAX] = "abcd";
        out_reset();
        redraw_tail(line, 4, 2);
        CHECK(strcmp(out, "cd " "\b\b\b") == 0,
              "redraw_tail reprints the tail, blanks one, and comes back");
    }

    /* ---- tab completion ---- */
    {
        char line[LINE_MAX]; size_t len, pos;

        strcpy(line, "upt"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "uptime") == 0 && len == 6 && pos == 6,
              "a unique prefix completes to the whole command");

        /* several candidates sharing no more letters: list them */
        strcpy(line, "c"); len = 1; pos = 1;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "c") == 0 && len == 1,
              "an ambiguous prefix leaves the line alone");
        CHECK(strstr(out, "clear") && strstr(out, "crash"),
              "and shows what it could have meant");

        /* several candidates that DO share letters: fill those in and
         * say nothing. `re` can only be reboot, `p` is poweroff or
         * persona or ps, but `po` is only poweroff */
        strcpy(line, "po"); len = 2; pos = 2;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "poweroff") == 0, "a unique-enough prefix fills in");

        /* an empty word must do nothing at all -- thats what help is for */
        strcpy(line, ""); len = 0; pos = 0;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len == 0 && len == 0,
              "a bare tab lists nothing, since `help` exists");

        /* a bare tab after a command that takes a filename should
         * answer, unlike a bare tab in the command position -- there is
         * no `help` listing files */
        strcpy(line, "cat "); len = 4; pos = 4;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len > 0, "cat<tab> with nothing typed offers something");

        strcpy(line, "run "); len = 4; pos = 4;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len > 0, "and so does run<tab>");

        /* run completes a nested path, which is where bin/hello lives */
        strcpy(line, "run bin/h"); len = 9; pos = 9;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "run bin/hello") == 0,
              "run completes bin/hello from a partial path");

        /* directories are not worth offering */
        strcpy(line, "cat bin"); len = 7; pos = 7;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat bin/hello") == 0,
              "completing `bin` skips the directory entry and finds the file");

        /* filenames after cat */
        strcpy(line, "cat mo"); len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat motd.txt") == 0 && pos == 12,
              "cat completes a filename out of the ramdisk");

        /* completing mid-line keeps whatever followed */
        strcpy(line, "cat mo done"); len = 11; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat motd.txt done") == 0,
              "and the rest of the line survives the insert");

        strcpy(line, "zzz"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "zzz") == 0 && out_len == 0,
              "an unmatchable prefix is left in peace");

        strcpy(line, "echo mo"); len = 7; pos = 7;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "echo mo") == 0 && out_len == 0,
              "only commands that take a file complete one -- echo gets nothing");

        strcpy(line, "echo "); len = 5; pos = 5;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len == 0, "nor does a bare tab after echo");

        /* an unknown command offers nothing after it either */
        strcpy(line, "zzz mo"); len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len == 0, "an unknown command has no filenames to offer");

        /* complete() must cope with a line the editor has not
         * terminated, which is the state it is really called in */
        memset(line, 'X', LINE_MAX);
        line[0]='c'; line[1]='a'; line[2]='t'; line[3]=' ';
        line[4]='m'; line[5]='o';
        len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat motd.txt") == 0,
              "an unterminated buffer still completes correctly");
    }

    /* ---- the new commands ---- */
    run("date");
    CHECK(strstr(out, "03:04:05") && strstr(out, "january") && strstr(out, "2026"),
          "date reports what the clock said");
    CHECK(strstr(out, "2nd") != NULL, "and gets the ordinal right");

    run("history");
    CHECK(out_len > 0, "history prints something");

    killed_id = -1; kill_answer = SCHED_KILL_OK;
    run("kill 3");
    CHECK(killed_id == 3 && strstr(out, "sea of souls"),
          "kill passes the id through and reports success");

    kill_answer = SCHED_KILL_BLOCKED;
    run("kill 4");
    CHECK(strstr(out, "waiting on something") != NULL,
          "a blocked thread is refused, with a reason");

    kill_answer = SCHED_KILL_PROTECTED;
    run("kill 1");
    CHECK(strstr(out, "wheel turning") != NULL, "idle is protected");

    killed_id = -1;
    run("kill notanumber");
    CHECK(killed_id == -1 && strstr(out, "not a thread id"),
          "a non-numeric id never reaches the scheduler");

    run("time echo hi");
    CHECK(strstr(out, "hi") && strstr(out, "ms]"),
          "time runs the command and reports how long it took");

    run("hexdump");
    CHECK(strstr(out, "hexdump <hex address>") != NULL, "hexdump explains itself");

    /* ---- the ramdisk ---- */
    run("ls");
    CHECK(strstr(out, "motd.txt") != NULL, "ls lists a file");
    CHECK(strstr(out, "in 5 files") != NULL,
          "and counts files, not the directory entries tar leaves behind");
    CHECK(strstr(out, "bin/hello") != NULL, "and the nested one");
    CHECK(strstr(out, "./") == NULL,
          "and prints paths you can actually retype -- no leading ./");

    run("cat motd.txt");
    CHECK(strstr(out, "Thou art I") != NULL, "cat prints the real file");

    run("cat nope.txt");
    CHECK(strstr(out, "no such file") != NULL, "and says so when it isnt there");

    run("cat hello");
    CHECK(strstr(out, "bin/hello") != NULL, "cat suggests the path too");

    run("cat");
    CHECK(strstr(out, "cat <file>") != NULL, "bare cat explains itself");

    /* the file that matters, by the exact path the shell must accept */
    run("cat bin/hello");
    CHECK(out_len > 0, "cat can reach bin/hello");

    /* cat takes several files at once */
    run("cat motd.txt motd.txt");
    {
        const char *first = strstr(out, "Thou art I");
        CHECK(first && strstr(first + 1, "Thou art I"),
              "cat concatenates, as named");
    }

    run("cat motd.txt nope.txt");
    CHECK(strstr(out, "Thou art I") && strstr(out, "no such file"),
          "and keeps going past one that is missing");

    run("dmesg");
    CHECK(strcmp(out, "<DMESG>") == 0, "dmesg reaches the log");

    /* a near miss gets a suggestion rather than a shrug */
    run("dmseg");
    CHECK(strstr(out, "didst thou mean 'dmesg'") != NULL,
          "one candidate by first letter earns a suggestion");
    run("qqq");
    CHECK(strstr(out, "try 'help'") != NULL,
          "no candidate falls back to pointing at help");

    /* running a program */
    ran_path = NULL; run_ok = true;
    run("run bin/hello");
    CHECK(ran_path && strcmp(ran_path, "bin/hello") == 0,
          "run passes the path through to the loader");

    run_ok = false;
    run("run junk");
    CHECK(strstr(out, "cannot run junk") && strstr(out, "not an elf"),
          "and reports why the loader refused");

    /* the mistake a person actually makes: the bare name of a file that
     * lives in a directory. we do not search paths, so say what they
     * meant rather than just refusing */
    run_error = USER_RUN_NO_SUCH_FILE;
    run("run hello");
    CHECK(strstr(out, "bin/hello") != NULL,
          "`run hello` points at bin/hello instead of just refusing");

    run("run nowhere");
    CHECK(strstr(out, "`ls`") != NULL,
          "and something with no near match points at ls and tab");
    run_error = "not an elf";
    run_ok = true;

    run("run");
    CHECK(strstr(out, "run <program>") != NULL, "bare run explains itself");

    /* ---- who we are ---- */
    run("arcana");
    CHECK(strstr(out, "COMPUTER ARCANA") && strstr(out, VERSION),
          "arcana names the arcana and the version");
    CHECK(strstr(out, "442") != NULL, "and how many symbols it carries");

    run("persona");
    CHECK(strstr(out, "velvet@tinyOS") != NULL, "persona has a header");
    CHECK(strstr(out, "Imaginary CPU") != NULL, "and reports the cpu");
    CHECK(strstr(out, "1280x800") != NULL, "and the resolution");
    CHECK(strstr(out, "4 threads") != NULL, "and the thread count");

    if (!failures) printf("all good\n");
    return failures;
}
