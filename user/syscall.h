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
#define SYS_OPEN   6
#define SYS_CLOSE  7
#define SYS_GETPID 8
#define SYS_SPAWN  9
#define SYS_WAIT   10
#define SYS_READDIR 11
#define SYS_GETUID 12
#define SYS_CREATE 13
#define SYS_CHDIR  14
#define SYS_GETCWD 15
#define SYS_MKDIR  16
#define SYS_RMDIR  17

/* the usual three, spoken for the way they are everywhere */
#define STDIN   0
#define STDOUT  1
#define STDERR  2

/* rcx and r11 are destroyed by the syscall instruction itself, and the
 * kernel may clobber anything the abi allows a call to */
static inline long syscall3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                      : "rcx", "r11", "memory");
    return ret;
}

/* five arguments needs the other two registers by name: the kernel's
 * entry stub expects the fourth in r10, because the syscall instruction
 * destroys rcx before anyone could have read it */
static inline long syscall5(long nr, long a0, long a1, long a2,
                            long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall2(long nr, long a0, long a1) { return syscall3(nr, a0, a1, 0); }
static inline long syscall1(long nr, long a0)          { return syscall3(nr, a0, 0, 0); }
static inline long syscall0(long nr)                   { return syscall3(nr, 0, 0, 0); }

static inline size_t ustrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline long write_fd(long fd, const void *buf, long len) {
    return syscall3(SYS_WRITE, fd, (long)buf, len);
}

static inline void write(const char *s) {
    write_fd(STDOUT, s, (long)ustrlen(s));
}

static inline long read_fd(long fd, void *buf, long len) {
    return syscall3(SYS_READ, fd, (long)buf, len);
}

/* ---- files -------------------------------------------------------- */

static inline long open(const char *path) {
    return syscall2(SYS_OPEN, (long)path, (long)ustrlen(path));
}
static inline long close(long fd) { return syscall1(SYS_CLOSE, fd); }

/* the nth name in the ramdisk, or -1 once there are no more */
static inline long readdir(long n, char *buf, long len) {
    return syscall3(SYS_READDIR, n, (long)buf, len);
}

/* the same, but of a directory somewhere. the ramdisk has no
 * directories to name, so it is the one you get when you name none */
static inline long readdir_at(long n, char *buf, long len, const char *path) {
    return syscall5(SYS_READDIR, n, (long)buf, len,
                    (long)path, (long)ustrlen(path));
}

/* open for writing, making the file if it is not there. only the disk
 * can do this -- the ramdisk is a tar file in read-only memory */
static inline long create(const char *path) {
    return syscall2(SYS_CREATE, (long)path, (long)ustrlen(path));
}

/* where I am, and moving. every relative name a program uses is read
 * from here, and it starts wherever whoever launched it was standing */
static inline long chdir(const char *path) {
    return syscall2(SYS_CHDIR, (long)path, (long)ustrlen(path));
}
static inline long getcwd(char *buf, long len) {
    return syscall2(SYS_GETCWD, (long)buf, len);
}
static inline long mkdir(const char *path) {
    return syscall2(SYS_MKDIR, (long)path, (long)ustrlen(path));
}
static inline long rmdir(const char *path) {
    return syscall2(SYS_RMDIR, (long)path, (long)ustrlen(path));
}

/* ---- other programs ----------------------------------------------- */

static inline long getpid(void) { return syscall0(SYS_GETPID); }
static inline long getuid(void) { return syscall0(SYS_GETUID); }

static inline long spawn(const char *path) {
    return syscall2(SYS_SPAWN, (long)path, (long)ustrlen(path));
}

/* blocks until that pid ends. returns the pid, or -1 if it was never
 * mine to wait for */
static inline long wait(long pid, int *code) {
    return syscall2(SYS_WAIT, pid, (long)code);
}

static inline long uptime(void)      { return syscall0(SYS_UPTIME); }
static inline void yield(void)       { syscall0(SYS_YIELD); }
/* returns -1 if an interrupt cut the sleep short */
static inline long sleep(long ms)    { return syscall1(SYS_SLEEP, ms); }
static inline void exit(long code)   { syscall1(SYS_EXIT, code); __builtin_unreachable(); }

/* just enough to print a number without a libc */
static inline int ustrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline void write_num(long v) {
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    write(&buf[i]);
}

#endif
