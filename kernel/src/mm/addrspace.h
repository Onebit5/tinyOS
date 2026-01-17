#ifndef MM_ADDRSPACE_H
#define MM_ADDRSPACE_H

#include <stdint.h>
#include <stdbool.h>

/* a private view of memory.
 *
 * up to now there was one set of page tables and every program loaded
 * into it, which meant they linked to the same addresses and only one
 * could exist at a time. now each gets its own pml4, and the difference
 * between them is only the lower half -- the upper half, where the
 * kernel and the direct map live, is shared by reference so the kernel
 * is reachable no matter whose tables are loaded. it has to be: the
 * stack I am standing on when I switch is up there. */

struct addrspace {
    uint64_t pml4;      /* physical address of the top level table */
};

/* a new space with the kernel half already visible and nothing else.
 * the kernel's own pml4 is passed in rather than fetched, which is the
 * same reason pmm_init_from_map takes a memory map: it lets the whole
 * thing be built and inspected on a host with no cpu involved */
struct addrspace *addrspace_create(uint64_t kernel_pml4);

/* free everything in the lower half -- the program's pages, its stack,
 * and the tables that described them. the upper half is shared and is
 * emphatically not mine to free.
 *
 * only safe once nothing is running on it. the reaper does this, from
 * another thread, after the switch away has already happened */
void addrspace_destroy(struct addrspace *as);

/* make this space the live one. passing NULL means the kernel's own.
 * writing cr3 flushes the whole tlb, so this does nothing when the
 * space is already loaded */
void addrspace_switch(struct addrspace *as);

/* how many frames a space is holding, for `ps` to report */
uint64_t addrspace_frames(struct addrspace *as);

#endif
