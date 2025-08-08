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

uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);

#endif
