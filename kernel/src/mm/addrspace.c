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

/* ---- forking ---------------------------------------------------------
 *
 * the page tables are copied and the pages underneath them are not.
 * every writable leaf in both spaces loses its write bit and gains
 * PTE_COW, which is a note to the fault handler saying the read-only is
 * a lie told on purpose */

static uint64_t free_level(uint64_t phys, int level);

/* one level of the walk, copied. `level` counts down: 4 is the pml4, 1
 * is a table whose entries are the actual pages */
static uint64_t copy_level(uint64_t phys, int level) {
    uint64_t fresh = pmm_alloc_pages(1);
    if (fresh == 0) {
        return 0;
    }
    uint64_t *out = table_at(fresh);
    uint64_t *in = table_at(phys);
    memset(out, 0, PAGE_SIZE);

    for (size_t i = 0; i < 512; i++) {
        if (!(in[i] & PTE_PRESENT)) {
            continue;
        }

        if (level == 1 || (in[i] & PTE_HUGE)) {
            /* a leaf. both sides give up the right to write to it and
             * both remember why, and the frame learns it has two
             * holders. the *parent's* entry is changed too -- a fork
             * where only the child is protected is a fork where the
             * parent quietly writes through the child's memory */
            uint64_t entry = in[i];
            if (entry & PTE_WRITE) {
                entry = (entry & ~PTE_WRITE) | PTE_COW;
                in[i] = entry;
            }
            out[i] = entry;
            pmm_ref(entry & PTE_ADDR_MASK);
            continue;
        }

        uint64_t child = copy_level(in[i] & PTE_ADDR_MASK, level - 1);
        if (child == 0) {
            /* out of memory partway down. what has been built so far is
             * a perfectly good little tree that nobody will ever point
             * at, so it goes back now -- the caller only ever sees a
             * zero and has nothing to free it with */
            free_level(fresh, level);
            return 0;
        }
        out[i] = child | (in[i] & ~PTE_ADDR_MASK);
    }

    return fresh;
}

struct addrspace *addrspace_fork(const struct addrspace *from,
                                 uint64_t kernel_pml4) {
    if (from == NULL) {
        return NULL;
    }
    /* sharing a frame means being able to count who holds it. without
     * that the first of the two to finish hands back a page the other
     * is still reading, so a fork that cannot be counted is a fork that
     * does not happen */
    if (!pmm_can_share()) {
        return NULL;
    }

    struct addrspace *as = addrspace_create(kernel_pml4);
    if (as == NULL) {
        return NULL;
    }

    const uint64_t *parent = table_at(from->pml4);
    uint64_t *mine = table_at(as->pml4);

    for (size_t i = 0; i < USER_PML4_ENTRIES; i++) {
        if (!(parent[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t child = copy_level(parent[i] & PTE_ADDR_MASK, 3);
        if (child == 0) {
            addrspace_destroy(as);
            return NULL;
        }
        mine[i] = child | (parent[i] & ~PTE_ADDR_MASK);
    }

    /* the parent's own entries just changed under it, and every core
     * that has run it may be holding a translation that still says
     * writable */
    smp_tlb_shootdown();
    return as;
}

/* find the leaf entry for an address, or NULL. no allocation: this runs
 * inside a fault handler and a fault handler that allocates page tables
 * is one that can fault */
static uint64_t *leaf_for(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *table = table_at(pml4_phys);
    int shift = 39;

    for (int level = 4; level > 1; level--) {
        size_t i = (virt >> shift) & 0x1ff;
        if (!(table[i] & PTE_PRESENT)) {
            return NULL;
        }
        if (table[i] & PTE_HUGE) {
            return &table[i];
        }
        table = table_at(table[i] & PTE_ADDR_MASK);
        shift -= 9;
    }
    return &table[(virt >> 12) & 0x1ff];
}

bool addrspace_fault(struct addrspace *as, uint64_t virt, bool write) {
    if (as == NULL || !write) {
        return false;       /* a read never faults on one of these */
    }

    uint64_t *pte = leaf_for(as->pml4, virt);
    if (pte == NULL || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW)) {
        return false;       /* not mine. a real fault */
    }

    uint64_t old = *pte & PTE_ADDR_MASK;

    /* the last holder does not need a copy of anything. it takes the
     * page back, which is what makes fork-then-exit cost nothing and
     * what stops a long chain of forks leaving copies behind */
    if (pmm_shares(old) == 0) {
        *pte = (*pte | PTE_WRITE) & ~PTE_COW;
        vmm_flush_page(virt);
        return true;
    }

    uint64_t fresh = pmm_alloc_pages(1);
    if (fresh == 0) {
        return false;       /* out of memory. it really is a fault now */
    }
    memcpy(table_at(fresh), table_at(old), PAGE_SIZE);

    *pte = fresh | ((*pte & ~PTE_ADDR_MASK) | PTE_WRITE);
    *pte &= ~PTE_COW;

    pmm_unref(old);
    vmm_flush_page(virt);
    return true;
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
            /* somebody else may still be reading it. unref only really
             * frees when the last holder lets go */
            pmm_unref(child);
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
