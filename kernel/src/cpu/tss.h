#ifndef CPU_TSS_H
#define CPU_TSS_H

#include <stdint.h>

/* the task state segment. long mode threw out hardware task switching
 * and kept the tss anyway, for two things we actually want:
 *
 *  - the IST, a set of known-good stacks the cpu switches to when
 *    certain exceptions fire, whether or not the current stack is
 *    usable. this is what makes stack overflow survivable: the guard
 *    page fault happens with rsp already inside the unmapped page, so
 *    the cpu cant push an exception frame, and without an IST that
 *    second failure becomes a double fault with nowhere to land and
 *    then a triple fault, which on real hardware is just a reboot.
 *
 *  - rsp0, the stack the cpu switches to on a ring 3 -> ring 0 trap.
 *    nothing uses it yet, but usermode will */

/* IST slots are numbered 1..7 in the idt (0 means "dont switch") */
#define IST_DOUBLE_FAULT 1

void tss_init(void);

/* what rsp0 should be when a given thread is on the cpu. usermode will
 * need this per thread; for now it is one stack and one comment */
void tss_set_rsp0(uint64_t rsp0);

#endif
