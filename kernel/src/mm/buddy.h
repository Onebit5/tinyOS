#ifndef MM_BUDDY_H
#define MM_BUDDY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* a buddy allocator, in place of a linear scan.
 *
 * the old pmm walked a bitmap looking for a run of free bits, which is
 * fine until the run is long or the memory is fragmented, and gets
 * slower the fuller it becomes. a buddy allocator keeps a free list per
 * size instead, so taking a block is following one pointer.
 *
 * the trick it is named for is on the way back. every block has exactly
 * one partner it could have been split from -- its buddy, found by
 * flipping one bit of its address -- so freeing means asking "is my
 * buddy also free?" and if so merging into one block of the next size
 * up, then asking again. large blocks reassemble themselves out of
 * small ones without anyone keeping a list of what was split from what.
 *
 * the cost is rounding: ask for five pages and you get eight. that
 * waste is real and is counted honestly, so `mem` shows it. */

#define BUDDY_MAX_ORDER 10          /* 2^10 frames = 4 MiB, the largest block */
#define BUDDY_NO_BLOCK  UINT64_MAX

/* how much memory the bitmaps need for a given number of frames */
uint64_t buddy_metadata_bytes(uint64_t frames);

/* set up over frames 0..frames, with `metadata` pointing at
 * buddy_metadata_bytes() of scratch. `to_virt` turns a frame number
 * into something writable -- the free lists are threaded through the
 * free pages themselves, which costs no memory at all because those
 * pages are by definition not being used for anything.
 *
 * everything starts allocated. give memory away with buddy_add_range */
void buddy_init(uint64_t frames, void *metadata,
                void *(*to_virt)(uint64_t frame));

/* hand a run of frames to the allocator, splitting it into the largest
 * aligned blocks that fit. this is how the memory map's usable regions
 * get in, and they are neither aligned nor powers of two */
void buddy_add_range(uint64_t first_frame, uint64_t count);

/* the smallest order that holds `frames`. deliberately not clamped: a
 * request too large for any block returns an order buddy_alloc refuses,
 * which is much better than quietly handing back something too small */
unsigned buddy_order_for(uint64_t frames);

/* is this exact block sitting on a free list? not a complete "is this
 * frame in use" -- a frame inside a larger free block is not itself on
 * any list -- but it catches the same block being freed twice, which is
 * the mistake worth catching */
bool buddy_is_free_block(uint64_t frame, unsigned order);

/* take a block of 2^order frames. returns the first frame, or
 * BUDDY_NO_BLOCK */
uint64_t buddy_alloc(unsigned order);

/* give one back. the order must be the one it was taken with, which is
 * why callers free with the same count they allocated */
void buddy_free(uint64_t frame, unsigned order);

uint64_t buddy_free_frames(void);

/* how many blocks are on each free list, for `mem` to show where the
 * memory actually is */
uint64_t buddy_blocks_at(unsigned order);

#endif
