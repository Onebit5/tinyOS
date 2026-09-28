#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

/* what this core can be told to do, and where it currently is.
 *
 * the two "where is it" calls are the ones worth explaining, because
 * they look like the kind of thing a portable kernel should not be
 * asking. they are asked for exactly two reasons and both are honest:
 * walking the call stack, and printing what the page tables say about
 * an address. every architecture has a stack and a chain of frames on
 * it; only the register names differ, which is the definition of
 * something belonging behind this line. */

#if defined(TINYOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. see
 * kernel/src/arch/none/README.md -- it exists to find out whether
 * anything above this line secretly needs a particular machine */
#  include "arch/none/cpu.h"
#elif defined(TINYOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/cpu.h"
#else
#  error "arch/cpu.h: no implementation for this architecture"
#endif

/* the contract, whatever implements it:
 *
 *   void cpu_relax(void)
 *      a hint that this is a spin, not work. it does not sleep and does
 *      not lower the clock -- it tells the core not to speculate a
 *      hundred iterations ahead of a value that is about to change out
 *      from under it, which is why every spin loop wants one.
 *
 *   void cpu_idle(void)
 *      stop until something happens. an interrupt ends it, so a caller
 *      must have interrupts *on* -- with them off this is where the
 *      machine stops forever, and that is a hang with no message.
 *
 *   void cpu_stop(void)  [noreturn]
 *      and the deliberate version of that: interrupts off, and stay
 *      here. for a panic, and for the end of a reset that did not take.
 *
 *   uint64_t cpu_frame_pointer(void)
 *   uint64_t cpu_stack_pointer(void)
 *      where this core is, for the backtrace and for `vmm`.
 */

#endif
