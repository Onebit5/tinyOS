#ifndef BOOT_H
#define BOOT_H

#include "philemon.h"

/* what philemon left behind. one struct, handed over in rdi the way any
 * function is called, describing everything about this machine that
 * could only be found out before long mode started.
 *
 * the kernel keeps a pointer to it rather than copying it, because it
 * sits in memory philemon marked as reclaimable -- which the kernel
 * does not reclaim until it is finished with all of this */

void boot_take_handoff(const struct ph_handoff *h);
const struct ph_handoff *boot_handoff(void);

/* the direct map offset, which everything needs and nothing should have
 * to go through the struct for */
uint64_t boot_hhdm(void);

#endif
