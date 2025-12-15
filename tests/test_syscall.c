/* the syscall dispatcher, and in particular what it refuses.
 *
 * every pointer arriving here is a claim made by ring 3, not a fact. a
 * program must not be able to make the kernel fault, or read kernel
 * memory, by lying about one -- so the refusals matter more than the
 * successes. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* ---- captured output ---- */
static char out[8192];
static size_t out_len;
static void out_reset(void) { out[0] = 0; out_len = 0; }
void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    out_len += vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);
    va_end(ap);
}

/* two page tables that disagree, which is the whole point.
 *
 * only the caller's space maps the program's memory -- the kernel's has
 * nothing down there at all. a check made against the wrong one says no
 * to every pointer that was ever going to be valid, which is exactly
 * what 0.1.0 shipped: programs printed nothing and exited with whatever
 * their failure path said. */
#define KERNEL_PML4 0x1000
#define CALLER_PML4 0x2000

/* ---- a fake address space ----
 * one page is user, one is present but supervisor-only, the rest is
 * nothing at all */
#include "mm/vmm.h"
static uint64_t user_page, kernel_page;
uint64_t vmm_kernel_pml4(void) { return KERNEL_PML4; }

static uint64_t user_extra;         /* another page the test calls the user's */
static uint64_t last_pml4_asked;

uint64_t vmm_flags(uint64_t pml4, uint64_t v) {
    last_pml4_asked = pml4;
    if (pml4 != CALLER_PML4) {
        return 0;                   /* the kernel's tables know none of this */
    }
    uint64_t p = v & ~0xfffull;
    if (p == user_page)   return PTE_PRESENT | PTE_USER | PTE_WRITE;
    if (user_extra && p == (user_extra & ~0xfffull))
                          return PTE_PRESENT | PTE_USER | PTE_WRITE;
    if (p == kernel_page) return PTE_PRESENT | PTE_WRITE;
    return 0;
}

#include <setjmp.h>
static jmp_buf jb;
static int exited, exit_code_seen;
void thread_exit(int code) { exited = 1; exit_code_seen = code; longjmp(jb, 1); }

/* the dispatcher asks who is calling, so there has to be somebody --
 * and that somebody has an address space of its own */
#include "sched/thread.h"
#include "mm/addrspace.h"
static struct addrspace my_space = { .pml4 = CALLER_PML4 };
static struct thread me = { .space = &my_space };
struct thread *sched_current(void) { return &me; }

uint64_t pit_uptime_ms(void) { return 1234; }
void sched_yield(void) { kprintf("<YIELD>"); }
void sleep_ms(uint64_t ms) { kprintf("<SLEEP %lu>", ms); }
static int next_key = 'x';
int input_getchar_blocking(void) { return next_key; }

/* syscall_init installs this in an msr; we never call it here */
void syscall_entry(void) { }

#include "sched/process.h"
#include "fs/ramdisk.h"

/* a one-file ramdisk, so `open` has something to find */
static const char motd[] = "hee-ho, from a file\n";
/* motd is readable by anyone; the secret is not. the mode comes out of
 * the tar header, and the kernel is the only one who gets to weigh it */
static const struct ramdisk_file rd[] = {
    { "./motd.txt",   motd, sizeof motd - 1, 0644 },
    { "./bin/",       "",   0,               0755 },
    { "./bin/cat",    motd, 4,               0755 },
    { "./secret.txt", motd, 4,               0600 },
};
bool ramdisk_may_read(const struct ramdisk_file *f, int uid) {
    return uid == 0 || (f->mode & 0004) != 0;
}
bool ramdisk_open(const char *name, struct ramdisk_file *out) {
    for (size_t i = 0; i < 4; i++) {
        const char *n = rd[i].name + 2;     /* past the ./ */
        if (strcmp(name, n) == 0) { *out = rd[i]; return true; }
    }
    return false;
}
bool ramdisk_stat(size_t i, struct ramdisk_file *out) {
    if (i >= 4) return false;
    *out = rd[i];
    return true;
}

/* the disk. the real one wants a sata controller, and what this file is
 * responsible for is not fat32 -- test_fat32 runs that against a real
 * image -- but the *routing*: that a path under the mount point reaches
 * the disk and one that is not reaches the ramdisk. so this stub
 * records what it was asked, and the tests check who got asked */
#include "fs/disk.h"
static const char disk_text[] = "on the disk\n";
static int disk_lookups, disk_creates;
static bool disk_is_ready = true;

bool disk_owns_path(const char *path) {
    return strncmp(path, "/disk", 5) == 0
        && (path[5] == '\0' || path[5] == '/');
}
bool disk_ready(void) { return disk_is_ready; }
bool disk_lookup(const char *path, struct disk_entry *out) {
    disk_lookups++;
    if (!disk_is_ready || strcmp(path, "/disk/hello.txt") != 0) {
        return false;
    }
    memset(out, 0, sizeof *out);
    strcpy(out->name, "hello.txt");
    out->size = sizeof disk_text - 1;
    out->cluster = 7;
    out->entry_sector = 100;
    return true;
}
bool disk_readdir(const char *path, size_t index, struct disk_entry *out) {
    if (!disk_is_ready || strcmp(path, "/disk") != 0 || index > 0) {
        return false;
    }
    memset(out, 0, sizeof *out);
    strcpy(out->name, "hello.txt");
    return true;
}
int64_t disk_read(uint32_t cluster, uint64_t size, uint64_t offset,
                  void *buf, uint64_t len) {
    (void)size;
    if (cluster != 7 || offset >= sizeof disk_text - 1) return 0;
    uint64_t left = (sizeof disk_text - 1) - offset;
    if (len > left) len = left;
    memcpy(buf, disk_text + offset, len);
    return (int64_t)len;
}
bool disk_create(const char *path, struct disk_entry *out) {
    disk_creates++;
    if (!disk_is_ready) return false;
    (void)path;
    memset(out, 0, sizeof *out);
    strcpy(out->name, "new.txt");
    out->entry_sector = 200;
    return true;
}
int64_t disk_write_at(struct disk_entry *e, uint64_t offset, const void *buf,
                      uint64_t len) {
    (void)buf;
    e->size = offset + len;
    return (int64_t)len;
}

/* the terminal, whose real version needs a scheduler to wake threads */
#include "drivers/tty.h"
static int foreground_pid = TTY_SHELL;
int tty_foreground(void) { return foreground_pid; }

/* the line discipline is the tty's own business and has its own suite.
 * here it only has to behave like one, so the syscall layer can be
 * checked for handing it the right things */
int64_t tty_read_line(int pid, char *buf, uint64_t len) {
    if (pid != foreground_pid || len == 0) return -1;
    if (process_take_interrupt(pid)) return -1;
    buf[0] = (char)next_key;
    return 1;
}

/* spawn and wait live in usermode.c, which needs a real cpu */
static int spawned_parent = -1;
static const char *spawned_path;
static int spawned_uid = -1;
int user_spawn(const char *path, int argc, const char *const argv[],
               int parent, int uid, const char **error) {
    (void)argc; (void)argv; (void)error;
    spawned_path = path; spawned_parent = parent; spawned_uid = uid;
    return 77;
}
static int wait_code = 5;
bool user_wait(int pid, int *code) {
    (void)pid; if (code) *code = wait_code; return true;
}

#include "cpu/syscall.h"
extern int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4);

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static int64_t call(uint64_t nr, uint64_t a0, uint64_t a1) {
    out_reset();
    return syscall_dispatch(nr, a0, a1, 0, 0, 0);
}

static int64_t call3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    out_reset();
    return syscall_dispatch(nr, a0, a1, a2, 0, 0);
}

/* read and write take a descriptor first now, the way they do
 * everywhere else. these keep the assertions readable */
static int64_t write_to(uint64_t fd, uint64_t ptr, uint64_t len) {
    return call3(SYS_WRITE, fd, ptr, len);
}
static int64_t read_from(uint64_t fd, uint64_t ptr, uint64_t len) {
    return call3(SYS_READ, fd, ptr, len);
}

int main(void) {
    /* the caller is a process, since half of these calls are about what
     * that process owns */
    me.pid = process_create("tester", 0, 0, false, 0);
    foreground_pid = me.pid;    /* it holds the terminal, mostly */

    /* a page a user program could legitimately own */
    char *page = aligned_alloc(4096, 8192);
    user_page = (uint64_t)page;
    kernel_page = (uint64_t)page + 4096;
    strcpy(page, "hello from ring 3");

    /* ---- pointers are judged by the CALLER's tables ----
     * this is the regression 0.1.0 introduced and 0.1.2 fixed: every
     * program got an address space of its own, but the checker kept
     * consulting the kernel's, where a program's memory is not mapped
     * at all. the symptom was every syscall taking a pointer quietly
     * returning -1, so programs printed nothing whatsoever */
    last_pml4_asked = 0;
    CHECK(write_to(FD_STDOUT, user_page, 4) == 4,
          "a valid user pointer is accepted");
    CHECK(last_pml4_asked == CALLER_PML4,
          "and it was the caller's page tables that were asked, not the "
          "kernel's -- which do not map user memory at all");

    /* a kernel thread has no space of its own and falls back to the
     * kernel's, which is right for it */
    me.space = NULL;
    last_pml4_asked = 0;
    (void)write_to(FD_STDOUT, user_page, 4);
    CHECK(last_pml4_asked == KERNEL_PML4,
          "a kernel thread is judged by the kernel's tables");
    me.space = &my_space;

    /* ---- the number actually selects the call ---- */
    CHECK(call(SYS_UPTIME, 0, 0) == 1234, "SYS_UPTIME returns the clock");
    CHECK(call(SYS_YIELD, 0, 0) == 0 && strstr(out, "<YIELD>"),
          "SYS_YIELD reaches the scheduler");
    CHECK(call(SYS_SLEEP, 250, 0) == 0 && strstr(out, "<SLEEP 250>"),
          "SYS_SLEEP passes its argument through");

    /* the exact failure that made a working kernel print nonsense: if
     * the entry stub does not shuffle registers, `nr` arrives holding
     * an address instead of a number */
    CHECK(call(0x400ada, 0, 0) == -1, "a nonsense number is refused");
    CHECK(strstr(out, "does not exist") != NULL, "and complained about");

    /* ---- writing ---- */
    CHECK(write_to(FD_STDOUT, user_page, 17) == 17, "SYS_WRITE writes user memory");
    CHECK(strcmp(out, "hello from ring 3") == 0, "and writes exactly it");

    CHECK(write_to(FD_STDOUT, user_page, 0) == 0, "a zero-length write is fine");

    /* ---- and what it refuses ---- */
    CHECK(write_to(FD_STDOUT, kernel_page, 8) == -1,
          "a page that is present but not user is refused -- this is the "
          "one that would let ring 3 read the kernel");
    CHECK(strstr(out, "refused a pointer") != NULL,
          "and the kernel says so rather than failing in silence, which is "
          "how this class of bug stays hidden");
    CHECK(strstr(out, "hello from ring 3") == NULL,
          "but not one byte of what it asked for gets through");

    CHECK(write_to(FD_STDOUT, (uint64_t)page + 0x100000, 8) == -1,
          "an unmapped pointer is refused");

    CHECK(write_to(FD_STDOUT, 0xffffffff80000000ull, 8) == -1,
          "a higher-half pointer is refused outright");
    CHECK(write_to(FD_STDOUT, 0xffff800000000000ull, 8) == -1,
          "including the very bottom of the kernel half");

    /* an absurd length becomes a short write, not a refusal -- and
     * crucially the clamped span is the one that gets validated, so it
     * can never be used to widen what the kernel will read */
    CHECK(write_to(FD_STDOUT, user_page, ~0ull) == 4096,
          "an absurd length is clamped to a short write");
    CHECK(out_len == 4096, "and exactly that much is written");

    /* the same trick aimed at kernel memory still gets nowhere */
    CHECK(write_to(FD_STDOUT, kernel_page, ~0ull) == -1,
          "clamping does not help a pointer that was never allowed");

    /* a span starting in user memory but running out of it */
    CHECK(write_to(FD_STDOUT, user_page + 4090, 16) == -1,
          "a write running off the end of its page is refused, not truncated");

    /* ---- reading ---- */
    next_key = 'q';
    char *buf = page;
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == 1, "reading fd 0 takes a key");
    CHECK(buf[0] == 'q', "from the keyboard");
    CHECK(read_from(FD_STDIN, kernel_page, 4) == -1, "and refuses kernel memory");
    CHECK(read_from(FD_STDIN, user_page, 0) == -1, "a zero-length read is refused");

    /* ---- only the foreground process may read the keyboard ----
     * a background program helping itself would take keys from whoever
     * is actually being typed at */
    foreground_pid = 999;               /* somebody else is at the front */
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == -1,
          "a background process is refused the keyboard");
    foreground_pid = me.pid;
    next_key = 'z';
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == 1,
          "and the foreground one is not");

    /* an interrupt delivered before a read means the read never starts */
    process_interrupt(me.pid);
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == -1,
          "a pending interrupt cuts a read short before it begins");
    CHECK(!process_interrupt_pending(me.pid), "and is consumed by it");

    /* a sleep says how it ended */
    CHECK(call(SYS_SLEEP, 1, 0) == 0, "an uninterrupted sleep returns 0");
    process_interrupt(me.pid);
    CHECK(call(SYS_SLEEP, 1, 0) == -1,
          "and an interrupted one says so, rather than pretending time passed");

    /* ---- writing somewhere that is not the console ---- */
    CHECK(write_to(3, user_page, 4) == -1,
          "nothing here is writable but the console, and a file says so");
    CHECK(write_to(FD_STDERR, user_page, 4) == 4, "stderr goes the same place");

    /* ---- files ---- */
    strcpy(page, "motd.txt");
    int64_t fd = call(SYS_OPEN, (uint64_t)page, 8);
    CHECK(fd >= FD_FIRST_FILE, "open finds a file and gives it a number");
    CHECK(call(SYS_OPEN, (uint64_t)page, 3) == -1, "a wrong name finds nothing");
    CHECK(call(SYS_OPEN, kernel_page, 8) == -1,
          "and a path the caller does not own is refused before it is read");

    char sink[64];
    memset(sink, 0, sizeof sink);
    user_extra = (uint64_t)sink;
    CHECK(read_from(fd, (uint64_t)sink, 6) == 6, "a short read works");
    CHECK(memcmp(sink, "hee-ho", 6) == 0, "and gives the first bytes");
    CHECK(read_from(fd, (uint64_t)sink, 6) == 6, "reading again continues");
    CHECK(memcmp(sink, ", from", 6) == 0, "from where the last one stopped");

    while (read_from(fd, (uint64_t)sink, 8) > 0) { }
    CHECK(read_from(fd, (uint64_t)sink, 8) == 0, "the end of a file reads zero");

    CHECK(call(SYS_CLOSE, fd, 0) == 0, "closing works");
    CHECK(call(SYS_CLOSE, fd, 0) == -1, "but only once");
    CHECK(read_from(fd, (uint64_t)sink, 4) == -1, "and the fd is dead after");

    /* ---- who am i, and starting others ---- */
    CHECK(call(SYS_GETPID, 0, 0) == me.pid, "getpid says who is asking");

    strcpy(page, "bin/thing");
    CHECK(call(SYS_SPAWN, (uint64_t)page, 9) == 77, "spawn returns the new pid");
    CHECK(spawned_parent == me.pid,
          "and records the caller as its parent, so only it may wait");

    int other = process_create("someone else's", 999, 0, false, 0);
    CHECK(call(SYS_WAIT, other, 0) == -1,
          "waiting for another process's child is refused -- otherwise the "
          "exit code would go to the wrong place");

    int mine = process_create("mine", me.pid, 0, false, 0);
    wait_code = 42;
    int codeout = 0;
    user_extra = (uint64_t)&codeout;
    CHECK(call(SYS_WAIT, mine, (uint64_t)&codeout) == mine, "ours works");
    CHECK(codeout == 42, "and fills in how it went");
    CHECK(call(SYS_WAIT, 4242, 0) == -1, "waiting for nothing is refused");

    /* ---- a uid is a thing the kernel checks, not a thing you claim ----
     * the mode came out of the tar header and the uid off the process.
     * neither is anything ring 3 can reach in and alter, which is the
     * only reason any of this means something */
    CHECK(call(SYS_GETUID, 0, 0) == 0, "getuid reports what the process runs as");

    strcpy(page, "secret.txt");
    CHECK(call(SYS_OPEN, (uint64_t)page, 10) >= FD_FIRST_FILE,
          "uid 0 may open a file nobody else may");

    /* the same call, from a process that is not the master */
    {
        int guest = process_create("guest", 0, 1000, false, 0);
        int was = me.pid;
        me.pid = guest;
        foreground_pid = guest;

        out_reset();
        CHECK(syscall_dispatch(SYS_OPEN, (uint64_t)page, 10, 0, 0, 0) == -1,
              "and a guest may not");
        CHECK(strstr(out, "may not read") != NULL, "and is told so plainly");

        strcpy(page, "motd.txt");
        CHECK(syscall_dispatch(SYS_OPEN, (uint64_t)page, 8, 0, 0, 0) >= FD_FIRST_FILE,
              "but may still read what is readable by anyone");

        CHECK(syscall_dispatch(SYS_GETUID, 0, 0, 0, 0, 0) == 1000,
              "and getuid says who it really is");

        /* a spawned child gets the uid of whoever started it -- a
         * program picking its own would make the whole idea decorative */
        strcpy(page, "bin/thing");
        (void)syscall_dispatch(SYS_SPAWN, (uint64_t)page, 9, 0, 0, 0);
        CHECK(spawned_uid == 1000, "a child inherits the uid it was started with");

        me.pid = was;
        foreground_pid = was;
        process_exited(guest, 0, 0);
        process_collect(guest, NULL);
    }
    strcpy(page, "motd.txt");

    /* ---- reading a directory ----
     * `ls` was the only command that needed something new to leave the
     * kernel: `open` can only answer about a name you already know */
    user_extra = (uint64_t)sink;
    memset(sink, 0, sizeof sink);
    CHECK(call3(SYS_READDIR, 0, (uint64_t)sink, sizeof sink) == 8,
          "readdir gives the first name");
    CHECK(strcmp(sink, "motd.txt") == 0,
          "with the leading ./ stripped, so it is a path open would take");

    CHECK(call3(SYS_READDIR, 2, (uint64_t)sink, sizeof sink) == 7,
          "and later ones by index");
    CHECK(strcmp(sink, "bin/cat") == 0, "including nested paths");

    CHECK(call3(SYS_READDIR, 99, (uint64_t)sink, sizeof sink) == -1,
          "past the end says so rather than inventing a name");
    CHECK(call3(SYS_READDIR, 0, kernel_page, 64) == -1,
          "and a buffer the caller does not own is refused");

    /* a name longer than the buffer is truncated, not written past */
    memset(sink, 0xaa, sizeof sink);
    CHECK(call3(SYS_READDIR, 0, (uint64_t)sink, 4) == 3,
          "a short buffer takes what fits");
    CHECK(strcmp(sink, "mot") == 0, "terminated, with nothing beyond it");

    /* ---- exit ---- */
    if (setjmp(jb) == 0) {
        syscall_dispatch(SYS_EXIT, 0, 0, 0, 0, 0);
        printf("FAIL: SYS_EXIT returned\n");
        failures++;
    }
    CHECK(exited, "SYS_EXIT ends the thread");

    if (!failures) printf("all good\n");
    return failures;
}
