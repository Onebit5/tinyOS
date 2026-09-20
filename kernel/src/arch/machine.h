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

#if defined(__x86_64__)
#include "arch/x86_64/machine.h"
#else
#error "arch/machine.h: no implementation for this architecture"
#endif

/* the contract, whatever implements it:
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
