#ifndef CPU_SYSCALL_H
#define CPU_SYSCALL_H

#include <stdint.h>

/* the door between ring 3 and here. numbers are ours, deliberately
 * small in number -- there is no libc out there to satisfy, only the
 * handful of things a program in this kernel could want */

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2
#define SYS_UPTIME 3
#define SYS_YIELD  4
#define SYS_SLEEP  5

/* wire up STAR/LSTAR/SFMASK and turn on EFER.SCE */
void syscall_init(void);

/* which kernel stack `syscall` should land on. the scheduler keeps this
 * pointed at the running thread, exactly like the tss rsp0 */
extern uint64_t syscall_kernel_rsp;

#endif
