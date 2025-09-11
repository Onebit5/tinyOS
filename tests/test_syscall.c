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

/* ---- a fake address space ----
 * one page is user, one is present but supervisor-only, the rest is
 * nothing at all */
#include "mm/vmm.h"
static uint64_t user_page, kernel_page;
uint64_t vmm_kernel_pml4(void) { return 0x1000; }
uint64_t vmm_flags(uint64_t pml4, uint64_t v) {
    (void)pml4;
    uint64_t p = v & ~0xfffull;
    if (p == user_page)   return PTE_PRESENT | PTE_USER | PTE_WRITE;
    if (p == kernel_page) return PTE_PRESENT | PTE_WRITE;
    return 0;
}

#include <setjmp.h>
static jmp_buf jb;
static int exited;
void thread_exit(void) { exited = 1; longjmp(jb, 1); }

uint64_t pit_uptime_ms(void) { return 1234; }
void sched_yield(void) { kprintf("<YIELD>"); }
void sleep_ms(uint64_t ms) { kprintf("<SLEEP %lu>", ms); }
static int next_key = 'x';
int input_getchar_blocking(void) { return next_key; }

/* syscall_init installs this in an msr; we never call it here */
void syscall_entry(void) { }

#include "cpu/syscall.h"
extern int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4);

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static int64_t call(uint64_t nr, uint64_t a0, uint64_t a1) {
    out_reset();
    return syscall_dispatch(nr, a0, a1, 0, 0, 0);
}

int main(void) {
    /* a page a user program could legitimately own */
    char *page = aligned_alloc(4096, 8192);
    user_page = (uint64_t)page;
    kernel_page = (uint64_t)page + 4096;
    strcpy(page, "hello from ring 3");

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
    CHECK(call(SYS_WRITE, user_page, 17) == 17, "SYS_WRITE writes user memory");
    CHECK(strcmp(out, "hello from ring 3") == 0, "and writes exactly it");

    CHECK(call(SYS_WRITE, user_page, 0) == 0, "a zero-length write is fine");

    /* ---- and what it refuses ---- */
    CHECK(call(SYS_WRITE, kernel_page, 8) == -1,
          "a page that is present but not user is refused -- this is the "
          "one that would let ring 3 read the kernel");
    CHECK(out_len == 0, "and nothing is printed from it");

    CHECK(call(SYS_WRITE, (uint64_t)page + 0x100000, 8) == -1,
          "an unmapped pointer is refused");

    CHECK(call(SYS_WRITE, 0xffffffff80000000ull, 8) == -1,
          "a higher-half pointer is refused outright");
    CHECK(call(SYS_WRITE, 0xffff800000000000ull, 8) == -1,
          "including the very bottom of the kernel half");

    /* an absurd length becomes a short write, not a refusal -- and
     * crucially the clamped span is the one that gets validated, so it
     * can never be used to widen what the kernel will read */
    CHECK(call(SYS_WRITE, user_page, ~0ull) == 4096,
          "an absurd length is clamped to a short write");
    CHECK(out_len == 4096, "and exactly that much is written");

    /* the same trick aimed at kernel memory still gets nowhere */
    CHECK(call(SYS_WRITE, kernel_page, ~0ull) == -1,
          "clamping does not help a pointer that was never allowed");

    /* a span starting in user memory but running out of it */
    CHECK(call(SYS_WRITE, user_page + 4090, 16) == -1,
          "a write running off the end of its page is refused, not truncated");

    /* ---- reading ---- */
    next_key = 'q';
    char *buf = page;
    CHECK(syscall_dispatch(SYS_READ, (uint64_t)buf, 1, 0, 0, 0) == 1,
          "SYS_READ fills a byte");
    CHECK(buf[0] == 'q', "with what the keyboard gave");
    CHECK(call(SYS_READ, kernel_page, 4) == -1, "and refuses kernel memory too");
    CHECK(call(SYS_READ, user_page, 0) == -1, "a zero-length read is refused");

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
