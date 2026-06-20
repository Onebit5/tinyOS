#ifndef FS_BCACHE_H
#define FS_BCACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* a cache of disk blocks, between the filesystem and the drive.
 *
 * every read went to the drive, one sector at a time, through a single
 * bounce buffer. that is worse than it sounds, because of *which*
 * sectors: walking a cluster chain reads the same handful of table
 * sectors over and over, and reading a directory reads the same entry
 * sector once per name in it. the drive was being asked the same
 * question hundreds of times and answering it identically.
 *
 * this slots in exactly where fat32's two function pointers already
 * are, which is the whole reason it can exist without the filesystem
 * knowing about it: fat32 was handed a way to move sectors, and it is
 * still handed a way to move sectors.
 *
 * writes are *write-back*, not write-through. a written block is marked
 * dirty and stays in memory, and the drive finds out later -- when the
 * block is evicted, when the flusher comes round, or when somebody says
 * `sync`. that is faster and it is a promise broken: until one of those
 * happens, what is on the disk is not what the machine believes. the
 * price is that power going out at the wrong moment loses work, which
 * is why unix has had `sync` since 1971 and why reboot and poweroff
 * call it here. */

#define BCACHE_SECTOR         512
#define BCACHE_BLOCK_SECTORS  8                     /* 4KiB, one page */
#define BCACHE_BLOCK_BYTES    (BCACHE_SECTOR * BCACHE_BLOCK_SECTORS)
#define BCACHE_BLOCKS         64                    /* 256KiB of cache */

/* how to reach the actual drive. the same two shapes fat32 uses, so
 * this can be dropped in between without either end noticing */
typedef bool (*bcache_in)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*bcache_out)(void *ctx, uint64_t lba, uint32_t count,
                           const void *buf);

/* point it at a drive. forgets everything it was holding, which is the
 * right thing when the drive underneath has changed and the wrong thing
 * to do without syncing first -- so mounting a second disk over a dirty
 * cache is a caller's mistake, not something to paper over here */
void bcache_init(bcache_in read, bcache_out write, void *ctx);

/* the two calls the filesystem makes. they have the signature of a
 * disk, because as far as anything above is concerned they are one */
bool bcache_read(void *ctx, uint64_t lba, uint32_t count, void *buf);
bool bcache_write(void *ctx, uint64_t lba, uint32_t count, const void *buf);

/* write every dirty block out. this is what `sync` means, and what
 * makes it worth having: until it returns, the disk does not hold what
 * the machine says it holds */
bool bcache_sync(void);

/* is there anything to lose? cheap enough to ask on a timer */
bool bcache_dirty(void);

/* what it has been doing, for `disk` to print. the hit rate is the only
 * honest measure of whether any of this was worth writing */
struct bcache_stats {
    uint64_t hits;          /* answered without touching the drive */
    uint64_t misses;        /* had to go and get it */
    uint64_t writes;        /* blocks written by somebody above */
    uint64_t writebacks;    /* blocks actually handed to the drive */
    uint64_t evictions;
    size_t   held;          /* blocks with something in them */
    size_t   dirty;
};

void bcache_get_stats(struct bcache_stats *out);

/* ---- the part with no drive in it ----
 *
 * which block an address belongs to, and where in it. split out because
 * an off-by-one here reads the right block and the wrong bytes, which
 * is the kind of wrong that looks like a filesystem bug */
static inline uint64_t bcache_block_of(uint64_t lba) {
    return lba / BCACHE_BLOCK_SECTORS;
}
static inline uint32_t bcache_offset_of(uint64_t lba) {
    return (uint32_t)(lba % BCACHE_BLOCK_SECTORS);
}

#endif
