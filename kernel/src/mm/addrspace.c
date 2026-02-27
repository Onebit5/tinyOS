#include "mm/addrspace.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "cpu/smp.h"
#include "lib/string.h"

/* the lower half is per-process, the upper half is everyone's. the
 * boundary is the halfway point of the pml4: entries 0..255 cover the
 * low canonical addresses, 256..511 the high ones */
#define USER_PML4_ENTRIES   256

static uint64_t live_pml4;      /* what cr3 currently holds */

static uint64_t *table_at(uint64_t phys) {
    return pmm_phys_to_virt(phys);
}

static struct slab_cache addrspace_cache;

struct addrspace *addrspace_create(uint64_t kernel_pml4) {
    slab_cache_init(&addrspace_cache, "addrspace", sizeof(struct addrspace));

    struct addrspace *as = slab_alloc(&addrspace_cache);
    if (as == NULL) {
        return NULL;
    }

    as->pml4 = vmm_new_address_space();
    if (as->pml4 == 0) {
        slab_free(as);
        return NULL;
    }

    /* share the kernel half by copying its top level entries. copying
     * the *entries* means both spaces point at the same lower tables,
     * so a change the kernel makes later -- splitting a huge page for a
     * guard page, say -- is seen by everyone. that only holds because
     * the kernel never adds a new top level entry after boot; it maps
     * everything it will ever need before the first program exists */
    const uint64_t *kernel = table_at(kernel_pml4);
    uint64_t *mine = table_at(as->pml4);
    for (size_t i = USER_PML4_ENTRIES; i < 512; i++) {
        mine[i] = kernel[i];
    }

    return as;
}

/* free a table's children and then the table itself. `level` counts
 * down: 4 is the pml4, 1 is a page table whose entries are the actual
 * pages. huge pages end the walk early, since they have no table under
 * them */
static uint64_t free_level(uint64_t phys, int level) {
    uint64_t freed = 0;
    uint64_t *table = table_at(phys);

    size_t entries = (level == 4) ? USER_PML4_ENTRIES : 512;
    for (size_t i = 0; i < entries; i++) {
        if (!(table[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t child = table[i] & PTE_ADDR_MASK;

        if (level == 1 || (table[i] & PTE_HUGE)) {
            pmm_free_pages(child, 1);   /* an actual page of the program */
            freed++;
        } else {
            freed += free_level(child, level - 1);
        }
        table[i] = 0;
    }

    pmm_free_pages(phys, 1);            /* and the table that held them */
    return freed + 1;
}

void addrspace_destroy(struct addrspace *as) {
    if (as == NULL) {
        return;
    }
    /* if this is somehow still loaded, get off it first. nothing should
     * reach here in that state, but a stale cr3 is not a thing to leave
     * to chance */
    if (live_pml4 == as->pml4) {
        addrspace_switch(NULL);
    }

    /* every core that ever ran a thread in this space may still be
     * holding translations out of tables I am about to hand back to the
     * allocator */
    smp_tlb_shootdown();

    free_level(as->pml4, 4);
    slab_free(as);
}

uint64_t addrspace_frames(struct addrspace *as) {
    if (as == NULL) {
        return 0;
    }
    uint64_t count = 0;
    const uint64_t *pml4 = table_at(as->pml4);

    for (size_t i = 0; i < USER_PML4_ENTRIES; i++) {
        if (!(pml4[i] & PTE_PRESENT)) {
            continue;
        }
        const uint64_t *pdpt = table_at(pml4[i] & PTE_ADDR_MASK);
        for (size_t j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) {
                continue;
            }
            if (pdpt[j] & PTE_HUGE) { count += 512 * 512; continue; }
            const uint64_t *pd = table_at(pdpt[j] & PTE_ADDR_MASK);
            for (size_t k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) {
                    continue;
                }
                if (pd[k] & PTE_HUGE) { count += 512; continue; }
                const uint64_t *pt = table_at(pd[k] & PTE_ADDR_MASK);
                for (size_t l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT) {
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

#ifndef TINYOS_HOSTED

void addrspace_switch(struct addrspace *as) {
    uint64_t want = (as != NULL) ? as->pml4 : vmm_kernel_pml4();

    /* writing cr3 throws away the whole tlb, so it is worth not doing
     * it when nothing has changed -- which is every switch between two
     * kernel threads, i.e. most of them */
    if (want == live_pml4) {
        return;
    }
    live_pml4 = want;
    asm volatile ("mov %0, %%cr3" : : "r"(want) : "memory");
}

#endif
