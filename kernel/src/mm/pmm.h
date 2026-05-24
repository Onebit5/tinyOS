#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "philemon.h"

/* physical memory manager. a buddy allocator underneath (see buddy.h):
 * free lists per block size, and blocks that put themselves back
 * together when both halves come home.
 *
 * one consequence leaks through this interface: sizes are rounded up to
 * a power of two, so asking for five pages costs eight. the count you
 * free with must be the count you allocated with, since that is what
 * says which list the block belongs on */

#define PAGE_SIZE 4096

void pmm_init(void);

/* the actual brains, split from the limine plumbing so host tests can
 * feed it a hand-made memory map */
void pmm_init_from_map(const struct ph_memmap_entry *entries, size_t count,
                       uint64_t hhdm);

/* allocate/free contiguous runs of frames. returns the physical address
 * of the first frame, or 0 when memory has run dry. frame 0 is never
 * handed out, so 0 is safe as the "no" answer */
uint64_t pmm_alloc_pages(size_t count);
void     pmm_free_pages(uint64_t phys, size_t count);

/* single-frame convenience wrappers */
uint64_t pmm_alloc(void);
void     pmm_free(uint64_t phys);

/* ---- frames with more than one owner --------------------------------
 *
 * copy on write means two address spaces pointing at one frame, and
 * whichever of them ends first must not hand it back while the other is
 * still reading it. so a frame can be shared, and freeing it only
 * really frees it when the last holder lets go.
 *
 * the count kept is of *extra* holders, so zero means one owner and the
 * ordinary path costs nothing but a byte's worth of lookup. a frame
 * nobody has shared behaves exactly as it always did. */

/* one more holder of this frame */
void pmm_ref(uint64_t phys);

/* one fewer. frees it only when the last one lets go. returns true if
 * that is what happened */
bool pmm_unref(uint64_t phys);

/* how many *extra* holders a frame has. 0 means one owner, which is
 * every frame that was never shared */
unsigned pmm_shares(uint64_t phys);

/* build the table. called once from pmm_init_from_map, where one core
 * is running and nothing is held -- it allocates, so it can never be
 * called from anywhere that already has the pmm's lock */
void pmm_shares_init(void);

/* may anything be shared? false on a machine too small to have paid for
 * the table, and a fork must refuse rather than go ahead: without it a
 * frame with two owners is freed by whichever finishes first */
bool pmm_can_share(void);

uint64_t pmm_share_bytes(void);

/* phys -> usable pointer, through the hhdm */
void *pmm_phys_to_virt(uint64_t phys);

/* where limine mirrored physical memory for me */
uint64_t pmm_hhdm_offset(void);

/* top of everything worth having in the direct map: the highest address
 * across every memmap entry that isnt reserved or broken. the reserved
 * holes way up at the 1TiB mark are deliberately excluded, mapping them
 * would cost megabytes of page tables for nothing */
uint64_t pmm_highest_address(void);

/* hand back the memory limine was using for itself: its page tables,
 * its stack, its structures. only safe once nothing of mine is still
 * standing on any of it -- in particular the boot thread has to be
 * gone, since its stack is in there.
 *
 * WARNING: this also frees the memory limine's *responses* live in, so
 * every `*_request.response` becomes a dangling pointer the moment
 * this returns. read what you need before calling it.
 *
 * returns how many bytes were recovered. calling twice is harmless. */
uint64_t pmm_reclaim_bootloader(void);

/* what the allocator's own bookkeeping costs, and how many free blocks
 * of each size there are -- the shape of the free memory, not just how
 * much of it there is. a machine with plenty free and none of it
 * contiguous is a machine about to fail a large allocation */
uint64_t pmm_metadata_bytes(void);
uint64_t pmm_blocks_at(unsigned order);

/* is this address covered by the allocator at all? only interesting
 * to the tests, which check that limine's memory is inside the map */
bool pmm_translate_is_tracked(uint64_t phys);

/* the most memory that has ever been in use at once. a current figure
 * tells you where you are; this tells you how close you came */
uint64_t pmm_peak_bytes(void);

uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);

#endif
