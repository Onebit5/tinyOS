#ifndef FS_FAT32_H
#define FS_FAT32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* fat32: a filesystem on a disk that survives a reboot.
 *
 * the ramdisk was a filesystem the way a filing cabinet is furniture --
 * a tar file handed over at boot, read-only, gone when the power goes.
 * this is the other thing. it lives on a disk, it can be written to,
 * and what you write is still there next time.
 *
 * fat is worth knowing because it is so nearly nothing. a boot sector
 * describes the layout. then a table with one entry per cluster, where
 * entry N holds the number of the cluster that follows N -- a linked
 * list with all the pointers gathered in one place, which is why it is
 * called a file allocation table. then the data. directories are not a
 * special kind of object at all: a directory is a file whose contents
 * happen to be 32-byte records.
 *
 * the fs never touches hardware. it is handed two functions that move
 * sectors, exactly like the pci scan is handed a way to read config
 * space -- which is what lets the tests run it against a real image
 * built by tools/mkfat.py, on a machine with no disk at all. */

#define FAT32_SECTOR   512
#define FAT32_NAME_MAX 128

/* how to reach the disk. returns false if the transfer failed */
typedef bool (*fat32_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*fat32_out)(void *ctx, uint64_t lba, uint32_t count,
                          const void *buf);

struct fat32 {
    fat32_io  read;
    fat32_out write;            /* NULL for a disk we may only read */
    void     *ctx;

    /* straight out of the boot sector */
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint32_t total_sectors;

    /* worked out from those, because everything else needs them */
    uint64_t first_data_sector;
    uint32_t cluster_count;

    char     label[12];
    bool     mounted;

    /* somewhere to put a sector while we look at it. one buffer, and
     * every path through here is short and holds no locks */
    uint8_t  scratch[FAT32_SECTOR];
};

/* what a directory entry says about a file. the two `entry_` fields are
 * where the record itself lives, which is how a write can go back and
 * correct the size afterwards */
struct fat32_file {
    char     name[FAT32_NAME_MAX];
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  attr;
    bool     is_dir;

    uint64_t entry_sector;
    uint32_t entry_offset;
};

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0f

/* read the boot sector and check it says what it should. `write` may be
 * NULL, in which case every writing call below refuses */
bool fat32_mount(struct fat32 *fs, fat32_io read, fat32_out write, void *ctx);

/* resolve a path like "notes/deep.txt". an empty path is the root */
bool fat32_lookup(struct fat32 *fs, const char *path, struct fat32_file *out);

/* walk a directory. index from 0 until it returns false. dot and dotdot
 * are skipped, since nothing above this has any use for them */
bool fat32_readdir(struct fat32 *fs, uint32_t dir_cluster, size_t index,
                   struct fat32_file *out);

/* read from anywhere in a file. returns bytes read, or -1. a short read
 * means the end of the file, as usual */
int64_t fat32_read(struct fat32 *fs, const struct fat32_file *f,
                   uint64_t offset, void *buf, uint64_t len);

/* write, growing the file and its cluster chain as needed, and
 * correcting the size in the directory afterwards. `f` is updated to
 * match what is now on disk */
int64_t fat32_write(struct fat32 *fs, struct fat32_file *f,
                    uint64_t offset, const void *buf, uint64_t len);

/* make a file in an existing directory. the name must fit 8.3, since we
 * write short entries only -- reading long names is one thing, minting
 * them is another, and a name we cannot store is better refused than
 * quietly mangled */
bool fat32_create(struct fat32 *fs, const char *path, struct fat32_file *out);

/* how much of the disk is spoken for, in clusters */
bool fat32_usage(struct fat32 *fs, uint32_t *used, uint32_t *total);

uint32_t fat32_cluster_bytes(const struct fat32 *fs);

#endif
