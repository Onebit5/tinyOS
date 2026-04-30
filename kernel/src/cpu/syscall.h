#ifndef CPU_SYSCALL_H
#define CPU_SYSCALL_H

#include <stdint.h>

/* the door between ring 3 and here. numbers are mine, deliberately
 * small in number -- there is no libc out there to satisfy, only the
 * handful of things a program in this kernel could want */

#define SYS_EXIT   0    /* (code)                 never returns      */
#define SYS_WRITE  1    /* (fd, buf, len)     -> bytes written        */
#define SYS_READ   2    /* (fd, buf, len)     -> bytes read           */
#define SYS_UPTIME 3    /* ()                 -> ms since boot        */
#define SYS_YIELD  4    /* ()                                         */
#define SYS_SLEEP  5    /* (ms)                                       */
#define SYS_OPEN   6    /* (path, len)        -> fd                   */
#define SYS_CLOSE  7    /* (fd)                                       */
#define SYS_GETPID 8    /* ()                 -> pid                  */
#define SYS_SPAWN  9    /* (path, len)        -> pid of the new one   */
#define SYS_WAIT   10   /* (pid, int *code)   -> pid, blocks          */
#define SYS_READDIR 11  /* (n,buf,len,path,plen) -> name length, or -1 */
#define SYS_GETUID 12   /* ()                 -> who this runs as      */
#define SYS_CREATE 13   /* (path, len)        -> fd, for writing        */
#define SYS_CHDIR  14   /* (path, len)        -> 0, or -1                */
#define SYS_GETCWD 15   /* (buf, len)         -> length written          */
#define SYS_MKDIR  16   /* (path, len)        -> 0, or -1                */
#define SYS_RMDIR  17   /* (path, len)        -> 0, or -1                */
#define SYS_UNLINK 18   /* (path, len)        -> 0, or -1                */
#define SYS_RENAME 19   /* (from,flen,to,tlen)-> 0, or -1                */
#define SYS_STAT   20   /* (path, len, struct user_stat *) -> 0, or -1   */

/* ---- the four a program drawing whole screens needs ----
 *
 * a shell needs none of these: it prints a prompt, reads a line and
 * prints an answer, and the console keeps the cursor where the printing
 * left it. an editor cannot work that way. it paints the entire screen,
 * puts the cursor somewhere in the middle of what it painted, and waits
 * for one keystroke rather than a line.
 *
 * getkey is the interesting one, because it is *also* how raw mode
 * arrives. there is no flag anywhere saying "this terminal is raw":
 * asking for a line gets the line discipline with its echo and its
 * backspace handling, and asking for a key gets the key. the two
 * questions are different, so they are different calls, and nothing has
 * to remember which mode anything is in */
#define SYS_GETKEY 21   /* ()                 -> one key, no echo       */
#define SYS_SCREEN 22   /* (uint32 *cols, uint32 *rows) -> 0           */
#define SYS_CURSOR 23   /* (col, row)         -> 0                      */
#define SYS_CLEAR  24   /* ()                 -> 0                      */

/* what a file is, for anyone who wants to know without reading it.
 *
 * this layout is duplicated in user/syscall.h, which is what an abi is:
 * two sides agreeing on where the fields sit, with nothing to enforce
 * it but the fact that they were written together. the padding is
 * explicit so that neither side's compiler gets to decide it */
struct user_stat {
    uint64_t size;
    uint32_t mode;
    uint32_t is_dir;
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
    uint8_t  pad;
};

#define SYSCALL_COUNT 25

/* wire up STAR/LSTAR/SFMASK and turn on EFER.SCE */
void syscall_init(void);

/* how many times each has been asked for, and what to call it. the
 * numbers are the cheapest possible picture of what a program actually
 * does -- one line of arithmetic per call, and afterwards you can say
 * with certainty which door gets used */
uint64_t syscall_times_called(unsigned nr);
const char *syscall_name(unsigned nr);

/* which kernel stack `syscall` should land on. the scheduler keeps this
 * pointed at the running thread, exactly like the tss rsp0 */
/* where this core's kernel stack is, for the entry stub to stand on.
 * per core, not global: the scheduler on one core must not be able to
 * rewrite the stack another core is about to use */
void syscall_set_kernel_rsp(uint64_t rsp);

#endif
