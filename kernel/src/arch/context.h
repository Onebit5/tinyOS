#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H

#include <stdint.h>

/* what a thread is, as far as the hardware is concerned.
 *
 * this is the header the rest of 0.2.20 was leading up to. the scheduler
 * is portable code -- a ring of structures, a quantum, a rule about who
 * runs next -- right up until the three moments where a *processor* has
 * to be involved, and until now it named all three by their x86 spelling:
 * a task state segment, a model-specific register holding a stack
 * pointer, and two entries in a descriptor table.
 *
 * none of those are ideas. they are how this machine spells three ideas
 * that every machine has. */

#if defined(__x86_64__)
#include "arch/x86_64/context.h"
#else
#error "arch/context.h: no implementation for this architecture"
#endif

/* the contract, whatever implements it:
 *
 *   void switch_context(uint64_t *save_sp, uint64_t *load_sp)
 *      stop being this thread and start being that one. everything
 *      about a parked thread lives on its own stack, so the whole of a
 *      saved thread is one word -- which is the trick the scheduler is
 *      built on rather than an implementation detail of it.
 *
 *   void context_set_kernel_stack(uint64_t top)
 *      when *this* thread next traps into the kernel, land here.
 *
 *      two separate registers on x86 because there are two ways in --
 *      an interrupt, which reads a task state segment, and a `syscall`,
 *      which reads an msr and would otherwise keep running on the
 *      caller's ring 3 stack. the scheduler does not need to know there
 *      are two, only that a thread about to run needs somewhere for the
 *      kernel to stand.
 *
 *   void context_enter_user(entry, stack_top, argc, argv)  [noreturn]
 *      leave the kernel, and do not come back. the selectors this needs
 *      on x86 were being passed in by the caller, which meant the
 *      portable half of the process code knew what a descriptor table
 *      was -- it does not need to, and now does not.
 */

#endif
