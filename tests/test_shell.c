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
void vmm_dump(uint64_t v) { kprintf("<VMM %#lx>", v); }
void kbacktrace(uint64_t rbp, uint64_t rip) { (void)rbp; (void)rip; kprintf("<BT>"); }
void system_poweroff(void) { kprintf("<POWEROFF>"); exit(0); }
uint64_t vmm_translate(uint64_t pml4, uint64_t v) { (void)pml4; (void)v; return v; }
#include "drivers/rtc.h"
void rtc_read(struct rtc_time *t) {
    t->second = 5; t->minute = 4; t->hour = 3;
    t->day = 2; t->month = 1; t->year = 2026;
}
#include "sched/sched.h"
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

        strcpy(line, "c"); len = 1; pos = 1;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "c") == 0 && len == 1,
              "an ambiguous prefix leaves the line alone");
        CHECK(strstr(out, "clear") && strstr(out, "crash"),
              "and shows what it could have meant");

        strcpy(line, "zzz"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "zzz") == 0 && out_len == 0,
              "an unmatchable prefix is left in peace");

        strcpy(line, "echo up"); len = 7; pos = 7;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "echo up") == 0 && out_len == 0,
              "completion does not fire on later words");
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

    if (!failures) printf("all good\n");
    return failures;
}
