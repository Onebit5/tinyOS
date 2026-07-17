#ifndef DRIVERS_PART_H
#define DRIVERS_PART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* a disk is not a filesystem.
 *
 * up to 0.2.14 this kernel believed otherwise: it asked each drive in
 * turn whether byte 1024 looked like an ext2 superblock, and mounted
 * whichever answered first. that works exactly as long as every disk
 * has one filesystem starting at sector zero, which is true of nothing
 * anybody actually uses.
 *
 * a real disk begins with a *table* saying where several filesystems
 * are. there are two of those tables in circulation and a machine has
 * to read both:
 *
 *   mbr, from 1983. four entries of sixteen bytes at offset 446 of the
 *   first sector, each with a start and a length in 32-bit sectors --
 *   which is where the two terabyte limit everybody used to complain
 *   about comes from.
 *
 *   gpt, which replaced it. a header at sector 1, an array of entries
 *   after it, 64-bit addresses, names, and crc32 over both so that a
 *   corrupt table can be *known* to be corrupt rather than followed.
 *
 * a gpt disk still carries an mbr, holding one entry of type 0xEE that
 * spans the whole disk. that is there so an old tool sees a full disk
 * rather than an empty one and declines to helpfully repartition it.
 * finding one means the real table is the gpt.
 *
 * none of this touches hardware. it is handed a function that reads
 * sectors, which is what lets the whole of it be tested against tables
 * built by hand on a machine with no disk at all. */

#define PART_SECTOR   512
#define PART_MAX      16
#define PART_NAME_MAX 36

enum part_scheme {
    PART_NONE = 0,      /* no table -- the whole drive, or nothing */
    PART_MBR,
    PART_GPT,
};

struct partition {
    uint64_t first_lba;
    uint64_t sectors;

    unsigned drive;         /* which drive it was found on */
    unsigned index;         /* 1-based, the way everybody numbers them */

    uint8_t  mbr_type;      /* 0 on a gpt disk */
    bool     bootable;      /* the mbr's active flag */

    /* gpt gives partitions names. mbr does not, so this is empty there
     * rather than invented */
    char     name[PART_NAME_MAX + 1];

    /* what the type says it holds, as far as I recognise it. this is
     * the table's opinion and not a fact -- what is actually there is
     * settled by trying to mount it */
    const char *kind;
};

/* how to read the drive being scanned */
typedef bool (*part_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/* read whichever table this drive has. returns how many partitions were
 * found, and sets *scheme to which kind of table said so.
 *
 * a drive with no table at all returns zero, which is not a failure:
 * plenty of images are one filesystem written straight to sector zero,
 * and the caller falls back to treating the whole drive as one */
size_t part_scan(part_io read, void *ctx, struct partition *out, size_t max,
                 enum part_scheme *scheme);

/* crc32, the ordinary reflected one. gpt covers both its header and its
 * entry array with it, which is the whole reason a gpt disk can tell a
 * corrupt table from a valid one and an mbr disk cannot */
uint32_t part_crc32(const void *data, size_t len);

/* what an mbr type byte is usually used for. a guess by convention --
 * the byte means whatever whoever wrote it meant */
const char *part_mbr_kind(uint8_t type);

#endif
