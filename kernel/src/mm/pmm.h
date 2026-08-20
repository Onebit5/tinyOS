#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>
#include <stddef.h>
#include "limine.h"

/* physical memory manager. a bitmap over every 4k frame of usable ram,
 * one bit each. frames come and go, the bitmap remembers */

#define PAGE_SIZE 4096

void pmm_init(void);

/* the actual brains, split from the limine plumbing so host tests can
 * feed it a hand-made memory map */
void pmm_init_from_map(struct limine_memmap_entry **entries, size_t count,
                       uint64_t hhdm);

/* allocate/free contiguous runs of frames. returns the physical address
 * of the first frame, or 0 when memory has run dry. frame 0 is never
 * handed out, so 0 is safe as the "no" answer */
uint64_t pmm_alloc_pages(size_t count);
void     pmm_free_pages(uint64_t phys, size_t count);

/* single-frame convenience wrappers */
uint64_t pmm_alloc(void);
void     pmm_free(uint64_t phys);

/* phys -> usable pointer, through the hhdm */
void *pmm_phys_to_virt(uint64_t phys);

/* where limine mirrored physical memory for us */
uint64_t pmm_hhdm_offset(void);

/* top of everything worth having in the direct map: the highest address
 * across every memmap entry that isnt reserved or broken. the reserved
 * holes way up at the 1TiB mark are deliberately excluded, mapping them
 * would cost megabytes of page tables for nothing */
uint64_t pmm_highest_address(void);

uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);

#endif
