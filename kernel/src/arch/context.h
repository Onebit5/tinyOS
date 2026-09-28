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

#if defined(TINYOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. see
 * kernel/src/arch/none/README.md -- it exists to find out whether
 * anything above this line secretly needs a particular machine */
#  include "arch/none/context.h"
#elif defined(TINYOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/context.h"
#else
#  error "arch/context.h: no implementation for this architecture"
#endif

/* the contract, whatever implements it:
 *
 *   uint64_t context_make_stack(uint64_t stack_top, void (*entry)(void))
 *      fabricate a stack that looks exactly like a thread already
 *      parked inside switch_context, waiting to be resumed -- and which
 *      will resume into `entry`. returns the saved stack pointer to
 *      hand switch_context the first time.
 *
 *      this is the one 0.2.21 added, and it is worth saying why,
 *      because it is the whole answer to whether 0.2.20's boundary was
 *      real. thread.c built that frame *itself*: six zeroes with a
 *      comment naming each one -- rbp, rbx, r12, r13, r14, r15 -- and a
 *      return address for switch_context's `ret` to land on.
 *
 *      neither checker could see it. there is no inline assembly and no
 *      x86 header; it is integers written into memory, and it compiles
 *      against an architecture that does nothing. it was found by
 *      writing the second architecture and discovering the frame is the
 *      wrong shape: aarch64 has ten callee-saved registers rather than
 *      six, and returns through x30, which is a register and not a slot
 *      on the stack for anybody to have written a zero into.
 *
 *      so the scheduler asks for a stack now instead of knowing how one
 *      is built, and what it knows is the only thing it ever needed to:
 *      that a new thread and a resumed thread must be indistinguishable.
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
