#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H

/* may interrupts happen right now.
 *
 * this is the single most-wanted thing in the kernel -- nineteen files
 * across mm, sched, fs, drivers and lib -- and until 0.2.20 every one of
 * them got it by including `cpu/interrupts.h`, an x86 header full of
 * apics and a struct listing rax through r15. they wanted four lines of
 * it and were handed the 8259 as well.
 *
 * so it lives here on its own. every architecture has a way to say "not
 * now" and none of them spell it the same, which is exactly what an arch
 * boundary is for. */

#if defined(__x86_64__)
#include "arch/x86_64/irq.h"
#else
#error "arch/irq.h: no implementation for this architecture"
#endif

/* the contract, whatever implements it:
 *
 *   uint64_t irq_save(void)
 *      turn interrupts off and hand back what they were, so this can
 *      nest. a blind disable/enable pair cannot -- the inner one puts
 *      them back on while the outer caller still needed them off, which
 *      is a bug that only shows up under load and only sometimes.
 *
 *   void irq_restore(uint64_t flags)
 *      put them back exactly as they were. not "on".
 *
 *   void irq_enable(void)
 *   void irq_disable(void)
 *      the unconditional pair. there are three callers between them and
 *      each has a reason it cannot use save/restore: a thread on its
 *      very first run has no saved state to restore, and a panic is
 *      never giving them back.
 */

#endif
