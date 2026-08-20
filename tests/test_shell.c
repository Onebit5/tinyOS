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

    /* ---- replace_line (what the arrow keys do to the screen) ---- */
    {
        char line[LINE_MAX] = "hello";
        size_t len = 5;
        out_reset();
        replace_line(line, &len, "ps");
        CHECK(strcmp(line, "ps") == 0 && len == 2, "replace_line swaps content");
        /* five erases (3 chars each), then the new text */
        CHECK(strncmp(out, "\b \b\b \b\b \b\b \b\b \b", 15) == 0,
              "replace_line erases every old character first");
        CHECK(strcmp(out, "\b \b\b \b\b \b\b \b\b \bps") == 0,
              "replace_line output is exactly erases then new text");
        CHECK(strstr(out, "ps") != NULL, "replace_line prints the new line");

        out_reset();
        replace_line(line, &len, "");
        CHECK(line[0] == 0 && len == 0, "replace_line can clear the line");
    }

    if (!failures) printf("all good\n");
    return failures;
}
