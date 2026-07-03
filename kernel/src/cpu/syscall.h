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
    uint32_t uid, gid;
    uint32_t is_symlink;
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
    uint8_t  pad;
};

#define SYS_FORK   25   /* ()  -> the child's pid, or 0 if you are it   */
#define SYS_MMAP   26   /* (len)              -> address, or 0          */
#define SYS_MUNMAP 27   /* (address)          -> 0, or -1               */
#define SYS_CHMOD  28   /* (path, len, mode)  -> 0, or -1               */
#define SYS_CHOWN  29   /* (path, len, uid, gid) -> 0, or -1            */
#define SYS_SYMLINK 30  /* (path,len,target,tlen) -> 0, or -1           */
#define SYS_READLINK 31 /* (path,len,buf,size) -> length, or -1         */

#define SYSCALL_COUNT 32

/* everything ring 3 was holding when it made the call, written down by
 * the entry stub in the order it pushes them.
 *
 * this exists for fork and for nothing else. a forked child has to come
 * back from a syscall it never made, holding exactly what its parent
 * held -- and the callee-saved half of that is in the cpu at the moment
 * of the call and gone a moment later, buried under some C prologue.
 *
 * the layout is the stack layout. changing either without the other is
 * a program that resumes with its registers shuffled, which is the kind
 * of bug that looks like the compiler being wrong */
struct user_regs {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r11;       /* the user's rflags, courtesy of syscall */
    uint64_t rcx;       /* the user's rip, likewise */
    uint64_t rsp;
};

/* fifteen registers, fifteen pushes. this catches a field appearing or
 * going away and cannot catch a reordering -- for that the only real
 * check is reading the two exit paths in
 * `objdump -d bin/tinyos --disassemble=syscall_entry` and
 * `--disassemble=fork_return` and seeing the same order twice */
_Static_assert(sizeof(struct user_regs) == 15 * 8,
               "user_regs and the pushes in syscall.asm have drifted apart");

/* leave for ring 3 through a frame rather than through a call. rax is
 * zeroed on the way out, because the only caller is a forked child and
 * that is how it finds out it is the child */
void fork_return(struct user_regs *frame);


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
