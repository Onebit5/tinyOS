#ifndef ARCH_MACHINE_H
#define ARCH_MACHINE_H

#include <stdbool.h>

/* the box, rather than the processor.
 *
 * separate from arch/cpu.h on purpose, and the split is not pedantry:
 * these three are the operations that are not the *architecture* at all,
 * they are the board around it. two machines with the same instruction
 * set stop and restart in entirely different ways, and one aarch64 board
 * has nothing whatever in common with another. keeping them apart means
 * a second architecture does not have to pretend its power switch is a
 * property of its instruction set. */

#if defined(TINYOS_ARCH_NONE)
/* an architecture that does nothing, for `make portable-check`. see
 * kernel/src/arch/none/README.md -- it exists to find out whether
 * anything above this line secretly needs a particular machine */
#  include "arch/none/machine.h"
#elif defined(TINYOS_ARCH_X86_64) || defined(__x86_64__)
#  include "arch/x86_64/machine.h"
#else
#  error "arch/machine.h: no implementation for this architecture"
#endif

/* the contract, whatever implements it:
 *
 *   void machine_bring_up_early(void)
 *      whatever has to exist before there is memory to allocate from:
 *      somewhere to print, a table of exception handlers, an interrupt
 *      controller, the devices that produce keystrokes.
 *
 *   void machine_bring_up_late(void)
 *      and whatever needs the allocators first: page tables, per-core
 *      state, the way in from userspace, the buses.
 *
 *   void machine_start_clock(void)
 *      the timer that preempts, and the other cores if there are any.
 *      after the scheduler exists, because the first tick will want to
 *      schedule something.
 *
 *      these three are what `kmain` used to be. it named the 8259, the
 *      gdt, the task state segment, the apics and the pci bus in the
 *      order they have to happen, which is fine for a kernel with one
 *      machine and is the whole of what a kernel with two cannot do.
 *      what is left in kmain is the part that is the same everywhere:
 *      take the handoff, find the memory, mount the filesystems, start
 *      init.
 *
 *   void machine_reset(void)  [noreturn]
 *      start the machine again. does not come back either way: if the
 *      reset does not take, this stops rather than returning to a caller
 *      that has already said goodbye.
 *
 *   bool machine_poweroff(void)
 *      switch it off. returns false if nothing answered, because that is
 *      genuinely possible and the caller has something useful to say
 *      about it -- "close the window" is better than a machine that sits
 *      there looking broken.
 *
 *   bool machine_key_pressed(void)
 *      did somebody press something, asked with interrupts off and never
 *      coming back. this exists for the panic screen and for nothing
 *      else. the keyboard driver is no help there: it is built entirely
 *      around an interrupt that will not arrive again, so the panic
 *      handler has to go and look for itself.
 */

#endif
