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
#define SYS_UNLINK 18
#define SYS_RENAME 19
#define SYS_STAT   20
#define SYS_GETKEY 21
#define SYS_SCREEN 22
#define SYS_CURSOR 23
#define SYS_CLEAR  24
#define SYS_FORK   25
#define SYS_MMAP   26
#define SYS_MUNMAP 27

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

/* remove a file. directories go through rmdir, which insists they are
 * empty first -- there is no recursive delete here and there is not
 * going to be one until something can be trusted to stop */
static inline long unlink(const char *path) {
    return syscall2(SYS_UNLINK, (long)path, (long)ustrlen(path));
}

/* give a file another name, which is also how it is moved: both are one
 * name replacing another, and neither copies a single byte */
static inline long rename(const char *from, const char *to) {
    return syscall5(SYS_RENAME, (long)from, (long)ustrlen(from),
                    (long)to, (long)ustrlen(to), 0);
}

/* what a file is, without opening it. the layout is the kernel's --
 * see struct user_stat in kernel/src/cpu/syscall.h, which is the other
 * half of this and has to be changed with it */
struct stat {
    uint64_t size;
    uint32_t mode;
    uint32_t is_dir;
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
    uint8_t  pad;
};

static inline long stat(const char *path, struct stat *out) {
    return syscall3(SYS_STAT, (long)path, (long)ustrlen(path), (long)out);
}

/* ---- other programs ----------------------------------------------- */

static inline long getpid(void) { return syscall0(SYS_GETPID); }
static inline long getuid(void) { return syscall0(SYS_GETUID); }

/* two of everything except the answer.
 *
 * the process this returns into is the same program, at the same
 * instruction, with the same open files and the same memory -- and one
 * difference, which is what comes back here. the parent gets the
 * child's pid; the child gets 0. that one number is how either half
 * knows which it is, and it is the whole interface.
 *
 * nothing is copied. both halves share every page until one of them
 * writes to it, so the memory only costs something at the moment it is
 * actually changed. -1 means it could not be done at all */
static inline long fork(void) { return syscall0(SYS_FORK); }

/* ---- memory, asked for rather than given ---------------------------
 *
 * everything a program had until now was decided before it started.
 * this is the other way round: ask for a length, get an address.
 *
 * nothing is actually made until it is touched, so asking for a
 * megabyte and using four bytes of it costs one page. that is what
 * makes asking for a lot reasonable rather than rude. 0 means no */
static inline void *mmap(long len) {
    return (void *)syscall1(SYS_MMAP, len);
}

/* by the address mmap handed back, not by any address inside it */
static inline long munmap(void *at) {
    return syscall1(SYS_MUNMAP, (long)at);
}

static inline long spawn(const char *path) {
    return syscall2(SYS_SPAWN, (long)path, (long)ustrlen(path));
}

/* blocks until that pid ends. returns the pid, or -1 if it was never
 * mine to wait for */
static inline long wait(long pid, int *code) {
    return syscall2(SYS_WAIT, pid, (long)code);
}

/* ---- painting a whole screen ---------------------------------------
 *
 * a program that draws its own screen needs three things a shell never
 * does: one key rather than a line, the size of what it is drawing on,
 * and a way to say where in it the cursor belongs.
 *
 * there is no "raw mode" to turn on anywhere. asking for a line gets
 * the line discipline, with its echo and its backspace handling;
 * asking for a key gets the key, unechoed. they are different
 * questions, so they are different calls, and nothing has to remember
 * which mode anything is in */

/* keys that are not characters come back above 0xff so they cannot be
 * mistaken for one. ctrl+letter arrives as the usual control codes --
 * ctrl+a is 1, the way it has been since 1963 */
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_DELETE 0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_PGUP   0x107
#define KEY_PGDN   0x108

/* blocks until a key is pressed. -1 if ctrl+c arrived instead */
static inline long getkey(void) { return syscall0(SYS_GETKEY); }

static inline long screen_size(uint32_t *cols, uint32_t *rows) {
    return syscall2(SYS_SCREEN, (long)cols, (long)rows);
}
static inline long cursor_to(long col, long row) {
    return syscall2(SYS_CURSOR, col, row);
}
static inline long clear_screen(void) { return syscall0(SYS_CLEAR); }

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
