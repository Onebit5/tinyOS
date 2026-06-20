/* host-side test for the shell's line splitting and command dispatch.
 * includes shell.c directly so the static helpers are reachable, and
 * stubs out every piece of kernel it leans on. kprintf is captured so
 * I can assert on exactly what the shell would have printed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

/* what the lock complains through */
void panic(const char *fmt, ...) {
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

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
/* both of these are declared noreturn, and they mean it -- so a stub
 * that simply returned would run straight into the trap the compiler
 * puts after it. it jumps back to whoever asked instead.
 *
 * what it must *not* do is exit(0), which is what it did until 0.2.13.
 * a stub that ends the process makes every assertion after it vacuous:
 * the suite stops, the runner sees a zero exit code, and the whole
 * thing reports success having run half of itself. that is a worse
 * failure than any bug it could have found, and it hid behind the fact
 * that nothing was testing reboot until something needed to */
#include <setjmp.h>
static jmp_buf stopped_here;
static int reboots, poweroffs;

void reboot(void) { kprintf("<REBOOT>"); reboots++; longjmp(stopped_here, 1); }
uint64_t pmm_total_bytes(void) { return 2046ull * 1024 * 1024; }
uint64_t pmm_used_bytes(void) { return 100ull * 1024; }
uint64_t pmm_free_bytes(void) { return 2045ull * 1024 * 1024; }
uint64_t kheap_total_bytes(void) { return 36 * 1024; }
uint64_t kheap_used_bytes(void) { return 512; }
void sched_dump(void) { kprintf("<PS>"); }
size_t process_count(void) { return 0; }
uint64_t pmm_peak_bytes(void) { return 0; }
uint64_t pmm_metadata_bytes(void) { return 64 * 1024; }
uint64_t pmm_blocks_at(unsigned order) { return order == 10 ? 511 : 0; }
struct slab_cache *slab_first_cache(void) { return NULL; }

/* the disk, which the shell only ever asks about */
#include "fs/disk.h"
bool disk_ready(void) { return true; }

/* a small tree, so completion has directories to descend into and
 * names that share prefixes to be careful about */
static const struct { const char *dir, *name; bool is_dir; } disk_tree[] = {
    { "/", "welcome.txt",     false },
    { "/", "notes",           true  },
    { "/", "hello.txt",       false },
    { "/", "big.bin",         false },
    { "/notes", "deep.txt",   false },
    { "/notes", "deeper.txt", false },
};
bool disk_lookup(const char *path, struct disk_entry *out) {
    for (size_t i = 0; i < sizeof disk_tree / sizeof disk_tree[0]; i++) {
        char full[160];
        snprintf(full, sizeof full, "%s%s%s", disk_tree[i].dir,
                 disk_tree[i].dir[1] == '\0' ? "" : "/", disk_tree[i].name);
        if (strcmp(full, path) != 0) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        return true;
    }
    return false;
}
int64_t disk_read(uint32_t c, uint64_t s, uint64_t o, void *b, uint64_t l) {
    (void)c; (void)s; (void)o; (void)b; (void)l; return -1;
}
bool disk_create(const char *path, struct disk_entry *out) {
    (void)path; (void)out; return false;
}
bool disk_mkdir(const char *path) { (void)path; return true; }
bool disk_rmdir(const char *path) { (void)path; return true; }
bool disk_unlink(const char *path) { (void)path; return true; }
bool disk_rename(const char *from, const char *to) {
    (void)from; (void)to; return true;
}
int64_t disk_write_at(struct disk_entry *e, uint64_t o, const void *b,
                      uint64_t l) {
    (void)e; (void)o; (void)b; (void)l; return -1;
}
bool disk_readdir(const char *path, size_t index, struct disk_entry *out) {
    size_t seen = 0;
    for (size_t i = 0; i < sizeof disk_tree / sizeof disk_tree[0]; i++) {
        if (strcmp(disk_tree[i].dir, path) != 0) continue;
        if (seen++ != index) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        return true;
    }
    return false;
}
/* the cache, as far as the shell is concerned: something that can be
 * dirty and can be told to stop being. what is under test here is that
 * the two ways this machine stops both write first -- a reboot that
 * does not sync throws away whatever had not reached the drive */
static bool cache_dirty = true;
static int  syncs;
static bool sync_ok = true;

bool disk_sync(void) { syncs++; if (sync_ok) cache_dirty = false; return sync_ok; }
bool disk_dirty(void) { return cache_dirty; }
void disk_cache_stats(struct bcache_stats *out) {
    memset(out, 0, sizeof *out);
    out->held = 3;
    out->hits = 90;
    out->misses = 10;
}

const char *disk_label(void) { return "TINYOS"; }
const char *disk_model(void) { return "QEMU HARDDISK"; }
uint64_t disk_bytes(void) { return 64ull * 1024 * 1024; }
uint32_t disk_cluster_bytes(void) { return 512; }
bool disk_usage(uint64_t *used, uint64_t *total) {
    *used = 32 * 1024; *total = 64ull * 1024 * 1024;
    return true;
}
uint64_t ahci_sectors(void) { return 131072; }

/* the processors, which the shell only ever asks about */
#include "cpu/smp.h"
static struct cpu fake_cpus[2] = {
    { .index = 0, .apic_id = 0, .reported_id = 0, .online = true,
      .bootstrap = true, .scheduling = true },
    { .index = 1, .apic_id = 1, .reported_id = 1, .online = true,
      .bootstrap = false, .scheduling = true },
};
size_t smp_cpu_count(void) { return 2; }
size_t smp_online_count(void) { return 2; }
const struct cpu *smp_cpu_at(size_t i) {
    return (i < 2) ? &fake_cpus[i] : NULL;
}
const char *sched_cpu_running(unsigned cpu) {
    return cpu == 0 ? "shell" : "idle1";
}
size_t sched_cores_scheduling(void) { return 2; }
uint64_t syscall_times_called(unsigned n) { (void)n; return 0; }
const char *syscall_name(unsigned n) { (void)n; return "x"; }
bool input_haskey(void) { return true; }
bool interrupts_on_apic(void) { return false; }
bool interrupts_use_ioapic(void) { return false; }
int input_getchar(void) { return 'q'; }
void klog_dump(void) { kprintf("<DMESG>"); }
static bool run_ok = true;
static const char *ran_path;
#include "sched/usermode.h"

/* a pipe a forked child inherits gains a holder rather than being
 * copied. nothing here forks, so it only has to exist */
struct pipe;
void pipe_share(struct pipe *p, bool writing) { (void)p; (void)writing; }

#include "sched/auth.h"
const char *const USER_RUN_NO_SUCH_FILE = "no such file in the ramdisk";

/* the accounts the shell reads at boot */
static const char passwd_text[] =
    "# a comment, and a blank line follow\n"
    "\n"
    "igor:velvet:0:master of the velvet room\n"
    "guest:guest:1000:a visitor\n";
static const char *run_error = "not an elf";
static bool ran_background;
static int ran_argc;
static const char *ran_arg1;
static int ran_uid = -1;
static bool ran_announce;
/* what the fake job comes back as: finished, or suspended by a ctrl+z
 * that the test says happened */
static bool run_stops;

bool user_run(const char *path, int argc, const char *const argv[],
              const char *cwd,
              int uid, bool background, bool announce, struct job *out,
              const char **error) {
    (void)cwd;
    ran_uid = uid;
    ran_announce = announce;
    ran_path = path;
    ran_argc = argc;
    ran_arg1 = (argc > 1) ? argv[1] : NULL;
    ran_background = background;

    memset(out, 0, sizeof *out);
    out->pgid = 42;
    out->pids[0] = 42;
    out->count = 1;
    out->stopped = run_stops && !background;

    if (run_ok) return true;
    *error = run_error;
    return false;
}

/* a job the test can decide is still going or not */
static bool job_is_alive;
static int  continued_pgid, continued_foreground;
static int  waits;

bool user_job_alive(const struct job *j) { (void)j; return job_is_alive; }
void user_job_collect(struct job *j) { (void)j; }

bool user_job_wait(struct job *j) {
    waits++;
    j->stopped = run_stops;
    return !run_stops;
}

void user_job_continue(struct job *j, bool foreground) {
    continued_pgid = j->pgid;
    continued_foreground = foreground ? 1 : 0;
}
/* what the shell handed to the pipeline, so a test can say the line was
 * chopped where the bars were and each piece resolved to a program */
static int pipe_count_seen;
static const char *pipe_paths[PIPELINE_MAX];
static int pipe_argcs[PIPELINE_MAX];
static const char *pipe_in[PIPELINE_MAX], *pipe_out[PIPELINE_MAX];
static bool pipe_append[PIPELINE_MAX];
static bool pipe_background;
static bool pipeline_ok = true;

bool user_pipeline(const struct stage *stages, int count, const char *cwd,
                   int uid, bool background, struct job *out,
                   const char **error) {
    (void)cwd; (void)uid;
    memset(out, 0, sizeof *out);
    out->pgid = 7;
    out->pids[0] = 7;
    out->count = count;
    out->stopped = run_stops && !background;
    pipe_count_seen = count;
    pipe_background = background;
    for (int i = 0; i < count && i < PIPELINE_MAX; i++) {
        pipe_paths[i] = stages[i].path;
        pipe_argcs[i] = stages[i].argc;
        pipe_in[i] = stages[i].in_path;
        pipe_out[i] = stages[i].out_path;
        pipe_append[i] = stages[i].append;
    }
    if (pipeline_ok) return true;
    *error = "no";
    return false;
}

size_t pipe_count(void) { return 0; }
void vmm_dump(uint64_t v) { kprintf("<VMM %#lx>", v); }
void kbacktrace(uint64_t rbp, uint64_t rip) { (void)rbp; (void)rip; kprintf("<BT>"); }
void system_poweroff(void) {
    kprintf("<POWEROFF>");
    poweroffs++;
    longjmp(stopped_here, 1);
}
uint64_t vmm_translate(uint64_t pml4, uint64_t v) { (void)pml4; (void)v; return v; }
#include "drivers/rtc.h"
void rtc_read(struct rtc_time *t) {
    t->second = 5; t->minute = 4; t->hour = 3;
    t->day = 2; t->month = 1; t->year = 2026;
}
void cpu_brand(char *buf) { strcpy(buf, "Imaginary CPU @ 1 Hz"); }
void console_size(size_t *c, size_t *r, size_t *w, size_t *h) {
    if (c) *c = 160;
    if (r) *r = 50;
    if (w) *w = 1280;
    if (h) *h = 800;
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

/* cmd_summon reads t->id off whatever I hand back, so hand back
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

    /* somewhere for a command that never returns to come back to */
    if (setjmp(stopped_here) == 0) {
        run_line(buf);
    }
}

int main(void) {
    auth_load(passwd_text, sizeof passwd_text - 1);

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

    /* a bar is its own word however it was typed. the two spellings
     * below are the same line, and anybody who has used a shell for ten
     * minutes expects that without ever having been told */
    check_split("cat x | head", 4, "cat", "x");
    check_split("cat x|head", 4, "cat", "x");
    check_split("cat x |head", 4, "cat", "x");
    check_split("|", 1, "|", NULL);
    check_split("echo hi > f", 4, "echo", "hi");
    check_split("echo hi>f", 4, "echo", "hi");
    check_split("echo hi>>f", 4, "echo", "hi");
    check_split("sort <a >b", 5, "sort", "<");
    {
        char buf[64] = "echo hi>>f";
        char *argv[ARGV_MAX];
        int n = split(buf, argv, ARGV_MAX);
        CHECK(n == 4 && strcmp(argv[2], ">>") == 0 && strcmp(argv[3], "f") == 0,
              "two arrows with no spaces are still one word, not two");
    }
    {
        char buf[64] = "a|b";
        char *argv[ARGV_MAX];
        int n = split(buf, argv, ARGV_MAX);
        CHECK(n == 3 && strcmp(argv[0], "a") == 0 && strcmp(argv[1], "|") == 0
              && strcmp(argv[2], "b") == 0,
              "a bar with no spaces round it still separates the two sides");
    }

    /* ---- pipelines ----
     *
     * what the shell owes a pipeline is the chopping and the resolving:
     * every stage has to be a real program *before* any of them starts,
     * because finding out halfway through leaves the earlier ones
     * already running and writing into a pipe with nobody at the end */

    pipe_count_seen = 0;
    run("cat motd.txt | head");
    CHECK(pipe_count_seen == 2, "a bar makes two stages");
    CHECK(pipe_paths[0] && strcmp(pipe_paths[0], "/bin/cat") == 0,
          "the first resolved to a program");
    CHECK(pipe_paths[1] && strcmp(pipe_paths[1], "/bin/head") == 0,
          "and so did the second");
    CHECK(pipe_argcs[0] == 2 && pipe_argcs[1] == 1,
          "with each stage keeping its own arguments and none of the "
          "other's");

    pipe_count_seen = 0;
    run("cat motd.txt | grep hee | wc -l");
    CHECK(pipe_count_seen == 3, "three commands make three stages");
    CHECK(pipe_argcs[1] == 2, "and the one in the middle keeps its argument");

    /* a bar joins two things, so there has to be something either side */
    pipe_count_seen = 0;
    run("| head");
    CHECK(pipe_count_seen == 0, "a pipeline starting with a bar runs nothing");
    CHECK(strstr(out, "either side") != NULL, "and says why");

    pipe_count_seen = 0;
    run("cat motd.txt |");
    CHECK(pipe_count_seen == 0, "and neither does one ending with a bar");

    pipe_count_seen = 0;
    run("cat motd.txt | | head");
    CHECK(pipe_count_seen == 0, "nor one with a gap in the middle");

    /* a name that is not a program stops the whole thing before any of
     * it starts */
    pipe_count_seen = 0;
    run("cat motd.txt | nonsuch");
    CHECK(pipe_count_seen == 0,
          "a stage that is not a program stops the pipeline before it "
          "begins");

    /* the shell prints with kprintf, straight at the screen -- it has
     * no stdout to hand anybody, so a builtin in a pipeline has to be
     * refused rather than quietly printing to the console while the
     * next stage waits for input that is never coming */
    pipe_count_seen = 0;
    run("ps | grep hello");
    CHECK(pipe_count_seen == 0, "a builtin in a pipeline runs nothing");
    CHECK(strstr(out, "builtin") != NULL, "and is told it is a builtin");

    /* ---- sync, and the two ways this machine stops ----------------
     *
     * before 0.2.13 a write reached the drive as it was made, so a
     * reboot lost nothing by definition. it is a cache now: a reboot
     * that does not write first throws away whatever had not reached
     * the drive, which on this machine is usually the file somebody
     * just spent a minute editing */

    cache_dirty = true;
    syncs = 0;
    run("sync");
    CHECK(syncs == 1, "sync writes what is waiting");
    CHECK(strstr(out, "written") != NULL, "and says so");

    run("sync");
    CHECK(strstr(out, "nothing waiting") != NULL,
          "and asking again says there is nothing to do");

    cache_dirty = true;
    sync_ok = false;
    run("sync");
    CHECK(strstr(out, "not what I believe") != NULL,
          "a drive that refuses is reported, since the disk and the "
          "machine now disagree and somebody should know");
    sync_ok = true;

    cache_dirty = true;
    syncs = 0;
    reboots = 0;
    run("reboot");
    CHECK(syncs == 1, "reboot writes first");
    CHECK(reboots == 1 && strstr(out, "<REBOOT>") != NULL, "and then reboots");

    cache_dirty = true;
    syncs = 0;
    poweroffs = 0;
    run("poweroff");
    CHECK(syncs == 1, "and so does poweroff");
    CHECK(poweroffs == 1, "before going out");

    cache_dirty = false;
    syncs = 0;
    run("reboot");
    CHECK(syncs == 0,
          "with nothing waiting, neither of them writes anything -- there "
          "is no point spinning up a drive to say nothing");

    /* ---- jobs -----------------------------------------------------
     *
     * a job is one typed line, however many processes that turned out
     * to be. the shell keeps them because it is the only thing that
     * knows what was *typed* -- the kernel has a group number and some
     * pids, and `[1]+ stopped  cat x | wc -l` needs the line */

    run("jobs");
    CHECK(strstr(out, "nothing is waiting") != NULL,
          "with nothing set aside, jobs says so");

    /* something put in the background is remembered, because it is
     * still there and you will want to name it later */
    run_stops = false;
    job_is_alive = true;
    run("counter &");
    CHECK(ran_background, "an & backgrounds it");
    CHECK(strstr(out, "[1]") != NULL, "and it gets a number");
    CHECK(strstr(out, "running") != NULL, "and is listed as running");
    CHECK(strstr(out, "counter &") != NULL,
          "under the line that was typed, which is the only reason the "
          "shell keeps jobs at all");

    run("jobs");
    CHECK(strstr(out, "[1]") && strstr(out, "counter"),
          "and `jobs` lists it afterwards");

    /* a foreground command that finishes is *not* remembered. writing
     * it down would only make a list of everything anybody ever did */
    run("echo hello");
    run("jobs");
    CHECK(strstr(out, "echo hello") == NULL,
          "something that ran and finished leaves no job behind");

    /* ctrl+z. the stub says the job came back stopped rather than done */
    run_stops = true;
    run("cat");
    CHECK(strstr(out, "stopped") != NULL, "a suspended job says so");
    CHECK(strstr(out, "[2]") != NULL, "and gets the next number");

    run("jobs");
    CHECK(strstr(out, "[1]") && strstr(out, "[2]"),
          "both are listed");
    CHECK(strstr(out, "[2]+") != NULL,
          "with a + on the one last touched, which is what a bare fg means");

    /* fg with no number means that one */
    continued_pgid = 0;
    continued_foreground = -1;
    run_stops = false;
    run("fg");
    CHECK(continued_pgid != 0, "a bare fg continues the marked job");
    CHECK(continued_foreground == 1, "and hands it the terminal");

    run("jobs");
    CHECK(strstr(out, "[2]") == NULL,
          "and a job that finished in the foreground is gone from the list");

    /* bg continues without the terminal, which is the whole difference */
    run_stops = true;
    run("cat");
    continued_foreground = -1;
    run("bg");
    CHECK(continued_foreground == 0,
          "bg continues it and does *not* hand over the terminal");
    run("jobs");
    CHECK(strstr(out, "running") != NULL, "and it is running again");

    /* a job number that was never handed out */
    run("fg 99");
    CHECK(strstr(out, "no job 99") != NULL, "an unknown job is refused by name");
    run("fg x");
    CHECK(strstr(out, "jobs") != NULL, "and so is something that is not a number");

    /* %1 is how everybody else spells it, so it works here too */
    continued_pgid = 0;
    run_stops = false;
    run("fg %1");
    CHECK(continued_pgid != 0, "%1 names a job the way it does everywhere");

    /* the ones still on the table finished while nobody was looking.
     * that gets reported at the next prompt rather than the instant it
     * happens, or it would scribble over whatever is half-typed */
    job_is_alive = false;
    out_reset();
    prompt();
    CHECK(strstr(out, "done") != NULL,
          "a background job that finished is reported at the next prompt");
    run("jobs");
    CHECK(strstr(out, "nothing is waiting") != NULL, "and then it is gone");

    /* ---- redirection ----
     *
     * the arrows are taken *out* of the arguments, not passed on. a
     * program has no business seeing them: `sort < a.txt` should look
     * to sort exactly like `sort` with something on standard input,
     * which is the entire idea */

    pipe_count_seen = 0;
    run("echo hello > out.txt");
    CHECK(pipe_count_seen == 1, "a redirect on its own is a one-stage pipeline");
    CHECK(pipe_out[0] && strcmp(pipe_out[0], "out.txt") == 0,
          "with the file taken off the line");
    CHECK(!pipe_append[0], "and a single arrow does not append");
    CHECK(pipe_argcs[0] == 2,
          "and the arrow and its filename gone from the arguments");

    pipe_count_seen = 0;
    run("echo hello >> out.txt");
    CHECK(pipe_append[0], "two arrows keeps what is already there");

    pipe_count_seen = 0;
    run("sort < in.txt > out.txt");
    CHECK(pipe_in[0] && strcmp(pipe_in[0], "in.txt") == 0, "both ends can move");
    CHECK(pipe_out[0] && strcmp(pipe_out[0], "out.txt") == 0, "at once");
    CHECK(pipe_argcs[0] == 1, "leaving just the command");

    /* an arrow with nothing after it is a sentence that stops halfway */
    pipe_count_seen = 0;
    run("cat >");
    CHECK(pipe_count_seen == 0, "an arrow with no filename runs nothing");
    CHECK(strstr(out, "nothing after") != NULL, "and says so");

    pipe_count_seen = 0;
    run("> out.txt");
    CHECK(pipe_count_seen == 0, "and a file with no command runs nothing");

    /* the middle of a pipeline already has both ends spoken for.
     * saying which of two things wins is worse than refusing */
    pipe_count_seen = 0;
    run("cat motd.txt | head > out.txt");
    CHECK(pipe_count_seen == 2, "the last stage may still redirect its output");
    CHECK(pipe_out[1] && strcmp(pipe_out[1], "out.txt") == 0, "to a file");

    pipe_count_seen = 0;
    run("cat motd.txt > out.txt | head");
    CHECK(pipe_count_seen == 0,
          "but a stage that already feeds another may not also redirect");
    CHECK(strstr(out, "already sends") != NULL, "and is told which it was");

    pipe_count_seen = 0;
    run("cat motd.txt | head < in.txt");
    CHECK(pipe_count_seen == 0,
          "nor may one that is already fed take its input from a file");

    /* & belongs to the line rather than to the last stage */
    pipe_count_seen = 0;
    run("cat motd.txt | head &");
    CHECK(pipe_count_seen == 2 && pipe_background,
          "a trailing & backgrounds the whole pipeline, not just its end");
    CHECK(pipe_argcs[1] == 1, "and is not handed to anybody as an argument");

    /* ---- dispatch ---- */
    run("help");
    CHECK(strstr(out, "summon") && strstr(out, "reboot"),
          "help lists the commands");

    /* echo and uptime are programs now, not builtins. typing them
     * should look no different, but it launches something in ring 3 */
    ran_path = NULL;
    run("echo thou art I");
    CHECK(ran_path && strcmp(ran_path, "/bin/echo") == 0,
          "echo is a program now, found on the search path");
    CHECK(ran_argc == 4, "and gets all its words");

    ran_path = NULL;
    run("uptime");
    CHECK(ran_path && strcmp(ran_path, "/bin/uptime") == 0,
          "and so is uptime");

    /* help is one list now: builtins and programs together, because
     * from where anybody is sitting there is one kind of thing here --
     * a word you type */
    out_reset();
    run("help");
    CHECK(strstr(out, "cd") != NULL, "help lists a builtin");
    CHECK(strstr(out, "echo") != NULL, "and a program");
    CHECK(strstr(out, "/bin") != NULL && strstr(out, "/boot/bin") != NULL,
          "and says where it looked, in order");
    CHECK(strstr(out, "help <name>") != NULL,
          "and points at where one thing is actually explained");
    /* the list is names only. what one of them means is `help <name>`,
     * and for a program that answer comes from the program */
    CHECK(strstr(out, "go somewhere; no argument") == NULL,
          "without a description beside every single one");

    /* ---- help for one thing ---- */

    out_reset();
    run("help cd");
    CHECK(strstr(out, "cd [directory]") != NULL,
          "a builtin explains itself out of the table");
    CHECK(strstr(out, "go somewhere") != NULL, "with what it is for");

    /* a program is asked rather than described: the shell runs it with
     * --help, because what it takes is declared inside it */
    ran_path = NULL;
    ran_arg1 = NULL;
    run("help echo");
    CHECK(ran_path && strcmp(ran_path, "/bin/echo") == 0,
          "a program is asked rather than described");
    CHECK(ran_arg1 && strcmp(ran_arg1, "--help") == 0,
          "by running it with --help, so the answer is its own");

    out_reset();
    run("help nonsense");
    CHECK(strstr(out, "not something you can type") != NULL,
          "and a name that is neither says so");

    /* a word with a slash in it is a path, taken exactly as written and
     * not searched for anywhere. `./x` is how you say "the one here" */
    ran_path = NULL;
    run("/boot/bin/echo hello");
    CHECK(ran_path && strcmp(ran_path, "/boot/bin/echo") == 0,
          "a full path runs exactly what it names");

    ran_path = NULL;
    run("bin/echo hello");
    CHECK(ran_path && strcmp(ran_path, "/bin/echo") == 0,
          "and a relative one is read from where I am standing");

    /* a name that is on no search path is not a command, however much
     * it looks like a file. the working directory is deliberately not
     * searched: a program left lying about must not become a verb */
    ran_path = NULL;
    out_reset();
    run("motd.txt");
    CHECK(ran_path == NULL, "a file that is not on the path is not a command");

    run("");
    CHECK(out_len == 0, "empty line does nothing at all");

    run("     ");
    CHECK(out_len == 0, "whitespace-only line does nothing");

    run("clear");
    CHECK(strcmp(out, "<CLEAR>") == 0, "clear reaches the console");

    run("ps");
    CHECK(strcmp(out, "<PS>") == 0, "ps reaches the scheduler");

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

        strcpy(line, "hex"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "hexdump") == 0 && len == 7 && pos == 7,
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

        /* a program completes in the command position, the same as a
         * builtin does -- they are the same kind of thing to type */
        strcpy(line, "upt"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "uptime") == 0, "a program name completes");

        /* ---- completing onto the disk ----
         * the ramdisk is flat and has no leading slash on anything, so
         * a word that starts with one is the disk's business. these are
         * the cases that did not work at all before: the candidates
         * only ever came from the ramdisk, so a path matched nothing */

        /* "/b" is ambiguous -- big.bin is also there -- so it must fill
         * in no further and list the two instead of picking one */
        strcpy(line, "ls /b"); len = 5; pos = 5;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "ls /b") == 0,
              "an ambiguous prefix completes no further");
        CHECK(out_len > 0, "and lists what it could have been");

        strcpy(line, "ls /bo"); len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "ls /boot/") == 0,
              "one more character reaches the mount point");

        strcpy(line, "ls /boot"); len = 8; pos = 8;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "ls /boot/") == 0,
              "and so does the mount point with no slash yet");

        strcpy(line, "cat /w"); len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat /welcome.txt") == 0,
              "a file on the disk completes to its whole path");

        /* a directory has to come back with a slash, so that tabbing
         * again carries on into it rather than stopping at a name that
         * cannot be opened */
        strcpy(line, "cat /n"); len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat /notes/") == 0,
              "a directory completes with a trailing slash");

        /* and then straight on into it */
        strcpy(line, "cat /notes/deep"); len = 15; pos = 15;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat /notes/deep") == 0,
              "two files sharing a prefix fill in no further");
        CHECK(out_len > 0, "and the pair gets listed instead");

        strcpy(line, "cat /notes/deeper"); len = 17; pos = 17;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat /notes/deeper.txt") == 0,
              "and one more character is enough to settle it");

        /* the two commands that had no completion at all before.
         * `write` was one of them until 0.2.9 removed it -- `echo x >
         * file` says the same thing with punctuation everybody already
         * knows, and one program fewer */
        strcpy(line, "grep x /h"); len = 9; pos = 9;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "grep x /hello.txt") == 0,
              "a filename anywhere in the arguments completes");

        strcpy(line, "ls /n"); len = 5; pos = 5;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "ls /notes/") == 0,
              "and so does ls");

        /* a bare tab after the mount point lists what is there */
        strcpy(line, "ls /"); len = 4; pos = 4;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len > 0, "a bare tab at the root lists it");

        /* a path that is not mine must not be answered with the disk */
        strcpy(line, "cat /nowhere/pass"); len = 17; pos = 17;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat /nowhere/pass") == 0,
              "a path into a directory that is not there completes to nothing");

        /* a relative path completes the same as an absolute one, which
         * is what `ls boot/<tab>` needs */
        strcpy(line, "ls notes/d"); len = 10; pos = 10;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "ls notes/deep") == 0,
              "a relative path completes against the tree, as far as two agree");

        strcpy(line, "cat notes/deeper"); len = 16; pos = 16;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat notes/deeper.txt") == 0,
              "and settles when one more character is given");

        /* run completes a nested path, which is where bin/hello lives */
        strcpy(line, "run bin/hell"); len = 12; pos = 12;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "run bin/hello") == 0,
              "run completes bin/hello from a partial path");

        /* `head` arrived in 0.2.8 and shares two letters with `hello`,
         * so `bin/h` is now genuinely ambiguous. it must stop at what
         * they agree on rather than picking whichever it found first --
         * a completion that guesses is worse than one that waits */
        strcpy(line, "run bin/h"); len = 9; pos = 9;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "run bin/he") == 0,
              "two programs sharing a prefix complete only as far as they "
              "agree");

        /* two programs live under bin/, so completing `bin` fills in as
         * far as they agree and stops rather than picking one */
        strcpy(line, "cat bin"); len = 7; pos = 7;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat bin/") == 0,
              "an ambiguous path completes to the shared prefix");

        strcpy(line, "cat bin/hel"); len = 11; pos = 11;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat bin/hello") == 0,
              "and one more character settles it");

        strcpy(line, "run bin/co"); len = 10; pos = 10;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "run bin/counter") == 0,
              "the other program completes too");

        /* the word after a bar is a command, not a file. completing it
         * against the working directory would offer exactly the wrong
         * list -- and this is the one place the shell has to know that
         * a pipeline is a sequence of commands rather than one long one */
        strcpy(line, "cat motd.txt | wc"); len = 17; pos = 17;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat motd.txt | wc") == 0,
              "a complete command after a bar stays as it is");

        strcpy(line, "cat motd.txt | so"); len = 17; pos = 17;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat motd.txt | sort") == 0,
              "and a partial one completes as a command, not as a filename");

        strcpy(line, "cat motd.txt |so"); len = 16; pos = 16;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat motd.txt |sort") == 0,
              "with no space after the bar either");

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

        /* a builtin that takes no filename offers none */
        strcpy(line, "bt mo"); len = 5; pos = 5;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "bt mo") == 0 && out_len == 0,
              "a builtin that takes no file completes nothing");

        /* but a program does, since the shell cannot know what it takes
         * and most of them take a filename */
        strcpy(line, "cat mo"); len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cat motd.txt") == 0,
              "and a program in bin/ completes filenames after it");

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

    kill_answer = SCHED_KILL_PROTECTED;
    run("kill 1");
    CHECK(strstr(out, "wheel turning") != NULL, "idle is protected");

    killed_id = -1;
    run("kill notanumber");
    CHECK(killed_id == -1 && strstr(out, "not a thread id"),
          "a non-numeric id never reaches the scheduler");

    run("time ps");
    CHECK(strstr(out, "<PS>") && strstr(out, "ms]"),
          "time runs the command and reports how long it took");

    run("hexdump");
    CHECK(strstr(out, "hexdump <hex address>") != NULL, "hexdump explains itself");

    /* ---- the ramdisk ----
     * ls and cat left the kernel in 0.1.4. the shell's job is now only
     * to find them in bin/ and hand over the arguments; what they print
     * is their own business, and tested where they live */
    ran_path = NULL;
    run("ls");
    CHECK(ran_path && strcmp(ran_path, "/bin/ls") == 0, "ls is a program now");

    ran_path = NULL;
    run("cat motd.txt");
    CHECK(ran_path && strcmp(ran_path, "/bin/cat") == 0, "and so is cat");

    /* typing a program by name wants its output, not a commentary on
     * it. running one deliberately is a demonstration, and the
     * ceremony is the point */
    CHECK(!ran_announce, "a command typed by name is not narrated");
    run("run bin/cat motd.txt");
    CHECK(ran_announce, "but one run deliberately is");







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
     * lives in a directory. I do not search paths, so say what they
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

    ran_background = true;
    run("run bin/hello");
    CHECK(!ran_background, "run waits for its program by default");

    run("run bin/hello &");
    CHECK(ran_background, "a trailing & puts it in the background");

    /* arguments reach the program, which is what let cat and echo
     * stop being kernel commands */
    run("run bin/cat motd.txt");
    CHECK(ran_argc == 2 && ran_arg1 && strcmp(ran_arg1, "motd.txt") == 0,
          "arguments after the program name are handed to it");

    run("run bin/cat motd.txt &");
    CHECK(ran_background && ran_argc == 2,
          "and the & is taken off rather than passed along as one");

    /* an unknown command is looked for on the search path, which is how
     * a command that moved keeps working without the shell knowing it
     * moved -- and how one on the disk becomes a command at all */
    ran_path = NULL;
    run("cat motd.txt");
    CHECK(ran_path && strcmp(ran_path, "/bin/cat") == 0,
          "an unknown command is looked for as a program");
    CHECK(ran_argc == 2 && ran_arg1 && strcmp(ran_arg1, "motd.txt") == 0,
          "with its arguments");

    ran_path = NULL;
    run("definitelynotathing");
    CHECK(ran_path == NULL, "and one that is not there is not run");

    /* ---- who I am ---- */
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
