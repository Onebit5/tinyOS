#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

/* the user side of the door. no libc out here -- these six inline
 * stubs are the whole runtime */

#include <stdint.h>
#include <stddef.h>

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2
#define SYS_UPTIME 3
#define SYS_YIELD  4
#define SYS_SLEEP  5

/* rcx and r11 are destroyed by the syscall instruction itself, and the
 * kernel may clobber anything the abi allows a call to */
static inline long syscall2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a0), "S"(a1)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall1(long nr, long a0) { return syscall2(nr, a0, 0); }
static inline long syscall0(long nr)          { return syscall2(nr, 0, 0); }

static inline size_t ustrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline void write(const char *s) {
    syscall2(SYS_WRITE, (long)s, (long)ustrlen(s));
}

static inline long uptime(void)      { return syscall0(SYS_UPTIME); }
static inline void yield(void)       { syscall0(SYS_YIELD); }
static inline void sleep(long ms)    { syscall1(SYS_SLEEP, ms); }
static inline void exit(long code)   { syscall1(SYS_EXIT, code); __builtin_unreachable(); }

/* just enough to print a number without a libc */
static inline void write_num(long v) {
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    write(&buf[i]);
}

#endif
