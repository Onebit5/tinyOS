#ifndef CPU_SYSCALL_H
#define CPU_SYSCALL_H

#include <stdint.h>

/* the door between ring 3 and here. numbers are ours, deliberately
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

/* wire up STAR/LSTAR/SFMASK and turn on EFER.SCE */
void syscall_init(void);

/* which kernel stack `syscall` should land on. the scheduler keeps this
 * pointed at the running thread, exactly like the tss rsp0 */
extern uint64_t syscall_kernel_rsp;

#endif
