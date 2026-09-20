#ifndef ARCH_MMU_H
#define ARCH_MMU_H

#include <stdint.h>
#include <stdbool.h>

/* the three things the memory manager needs the *hardware* told.
 *
 * everything else about paging in this kernel is arithmetic over
 * structures in memory, and arithmetic is portable. these three are not:
 * they are the moments where what was written down has to be believed by
 * a processor.
 *
 * note what is not here. building the tables, walking them, splitting a
 * huge page, counting what is mapped -- none of that belongs behind this
 * line even though all of it is four-level and x86-shaped today. the
 * boundary is drawn at "the hardware has to be told", because that is
 * the part a second architecture genuinely does differently rather than
 * the part it merely spells differently. */

#if defined(__x86_64__)
#include "arch/x86_64/mmu.h"
#else
#error "arch/mmu.h: no implementation for this architecture"
#endif

/* the contract, whatever implements it:
 *
 *   void mmu_load_table(uint64_t phys_root)
 *      run on these tables from the next instruction onward. the whole
 *      translation cache goes with it, which is why callers bother to
 *      check whether anything actually changed first.
 *
 *   void mmu_flush_page(uint64_t virt)
 *      one page's translation is stale. the processor caches these and
 *      nothing in the hardware notices that the table underneath moved,
 *      so a mapping changed without this is a mapping that is sometimes
 *      the old one -- for as long as the cache feels like it.
 *
 *   void mmu_enforce_write_protect(void)
 *      make read-only mean read-only *for the kernel too*.
 *
 *      this reads like an x86 wart and is really a portable question
 *      asked once: is W^X something the processor will hold me to, or
 *      only something I have written down. on x86 the answer is no by
 *      default and this turns it on; on an architecture where the answer
 *      is already yes, this is an empty function -- which is the right
 *      shape for it, and is why the *intent* is the name rather than the
 *      bit it happens to set here.
 */

#endif
