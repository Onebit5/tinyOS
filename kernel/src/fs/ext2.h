#ifndef FS_EXT2_H
#define FS_EXT2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "fs/fat32.h"       /* for struct fat32_time, see below */

/* ext2: a filesystem with opinions.
 *
 * fat records no ownership and no permissions. that is not an oversight
 * in fat, it is what fat is for -- but it meant everything on the disk
 * here was 0644 owned by root by decree, and `chmod` had nowhere to
 * write an answer down. a filesystem that has them was the only way
 * past that, and while there is one there may as well be symlinks and
 * timestamps that are not rounded to two seconds.
 *
 * this is revision 1 with 1 KiB blocks and 128-byte inodes. the shape
 * of it is worth knowing because almost every unix filesystem since has
 * been a variation on it:
 *
 *   a superblock says how big everything is.
 *   the disk is cut into block groups, each with a bitmap of its blocks,
 *   a bitmap of its inodes, and a table of the inodes themselves.
 *   an inode is a file -- its mode, its owner, its size, its times, and
 *   fifteen block numbers. a name is not in there at all.
 *   a directory is a file whose contents are (inode, name) pairs.
 *
 * that last pair of facts is the whole difference from fat. in fat a
 * file *is* its directory entry, so a file has exactly one name and
 * ownership has nowhere to live. here the name and the file are
 * different objects, which is why unix has hard links, why permissions
 * belong to the file rather than to the name, and why renaming
 * something across directories moves nothing.
 *
 * twelve of the fifteen block numbers are the first twelve blocks. the
 * thirteenth points at a block *of block numbers*, the fourteenth at a
 * block of those, the fifteenth one deeper again. so a small file costs
 * nothing extra and a large one costs a walk -- which is the trade every
 * filesystem of the era made, and the reason ext4 has extents.
 *
 * the driver touches no hardware. it is handed two functions that move
 * sectors, exactly as fat32 is, which is what lets the tests run it
 * against a real image built by tools/mkext2.py -- and lets the block
 * cache sit underneath without either end knowing. */

#define EXT2_SECTOR       512
#define EXT2_MAX_BLOCK    4096          /* the largest I will mount */
#define EXT2_NAME_MAX     255
#define EXT2_ROOT_INO     2

/* the one feature I set, and the one I understand: a directory entry
 * carries the kind of thing it names, so listing a directory does not
 * mean reading an inode per name. anything else in the incompat field
 * means a filesystem I would be guessing at, and guessing at a
 * filesystem is how you write over somebody's data */
#define EXT2_INCOMPAT_FILETYPE  0x0002
#define EXT2_INCOMPAT_KNOWN     EXT2_INCOMPAT_FILETYPE

/* the mode bits, which are unix's and are the point of the exercise */
#define EXT2_S_IFMT   0xf000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xa000

#define EXT2_DIRECT   12

typedef bool (*ext2_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*ext2_out)(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf);

/* what time it is, for the three ext2 keeps. seconds since 1970, which
 * is what the format stores -- the broken-down version is worked out on
 * the way out, so that everything above this sees one shape of
 * timestamp whichever filesystem it came from */
typedef uint32_t (*ext2_clock)(void);

struct ext2 {
    ext2_io     read;
    ext2_out    write;          /* NULL for a filesystem I may only read */
    ext2_clock  clock;
    void       *ctx;

    /* out of the superblock */
    uint32_t block_size;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_ino;
    uint32_t groups;
    uint32_t gdt_block;         /* where the descriptor table begins */

    char     label[17];
    bool     mounted;

    /* somewhere to put a block while I look at it, and a second for
     * walking indirect blocks -- which means holding two at once, and
     * is the whole reason there are two */
    uint8_t  scratch[EXT2_MAX_BLOCK];
    uint8_t  indirect[EXT2_MAX_BLOCK];
};

/* what an inode says about itself. `ino` is the identity: two names for
 * one file have the same one, which is a sentence fat cannot say */
struct ext2_file {
    uint32_t ino;
    uint32_t mode;              /* type and permissions together */
    uint32_t uid, gid;
    uint64_t size;
    uint32_t links;

    bool     is_dir;
    bool     is_symlink;

    /* the three ext2 keeps, broken down so that everything above sees
     * one shape of timestamp whichever filesystem it came from */
    struct fat32_time accessed, modified, created;

    char     name[EXT2_NAME_MAX + 1];
};

/* read the superblock and check I understand it. `write` may be NULL,
 * in which case every writing call below refuses */
bool ext2_mount(struct ext2 *fs, ext2_io read, ext2_out write, void *ctx);

/* where the time comes from. optional; without it, written timestamps
 * stay whatever they were */
void ext2_set_clock(struct ext2 *fs, ext2_clock clock);

/* resolve a path like "notes/deep.txt". an empty path is the root.
 * symlinks along the way are followed, up to a limit -- a loop of them
 * is a filesystem asking to be walked forever */
bool ext2_lookup(struct ext2 *fs, const char *path, struct ext2_file *out);

/* the same, without following a symlink at the end of it. this is what
 * `ls -l` and `rm` want: the link itself rather than what it points at */
bool ext2_lookup_nofollow(struct ext2 *fs, const char *path,
                          struct ext2_file *out);

/* where a symlink points. false if it is not one */
bool ext2_readlink(struct ext2 *fs, const struct ext2_file *f, char *out,
                   size_t size);

/* walk a directory. index from 0 until it returns false. dot and dotdot
 * are skipped, since nothing above this has any use for them */
bool ext2_readdir(struct ext2 *fs, uint32_t dir_ino, size_t index,
                  struct ext2_file *out);

/* read from anywhere in a file. returns bytes read, or -1 */
int64_t ext2_read(struct ext2 *fs, const struct ext2_file *f,
                  uint64_t offset, void *buf, uint64_t len);

/* write, growing the file and allocating blocks as needed. `f` is
 * updated to match what is now on the disk */
int64_t ext2_write(struct ext2 *fs, struct ext2_file *f,
                   uint64_t offset, const void *buf, uint64_t len);

/* make things. the parent has to exist; the name has no 8.3 rule to
 * obey, which is most of why this exists */
bool ext2_create(struct ext2 *fs, const char *path, uint32_t mode,
                 uint32_t uid, uint32_t gid, struct ext2_file *out);
bool ext2_mkdir(struct ext2 *fs, const char *path, uint32_t mode,
                uint32_t uid, uint32_t gid);
bool ext2_symlink(struct ext2 *fs, const char *path, const char *target,
                  uint32_t uid, uint32_t gid);

/* and unmake them */
bool ext2_unlink(struct ext2 *fs, const char *path);
bool ext2_rmdir(struct ext2 *fs, const char *path);

/* a name moves; the file does not. within one filesystem only, which is
 * what rename has always meant */
bool ext2_rename(struct ext2 *fs, const char *from, const char *to);

/* the things fat had nowhere to write down */
bool ext2_chmod(struct ext2 *fs, const char *path, uint32_t mode);
bool ext2_chown(struct ext2 *fs, const char *path, uint32_t uid, uint32_t gid);

/* how much of it is spoken for */
bool ext2_usage(struct ext2 *fs, uint64_t *used_bytes, uint64_t *total_bytes);

uint32_t ext2_block_bytes(const struct ext2 *fs);

/* ---- the part with no disk in it ----
 *
 * seconds since 1970 into a date somebody can read. split out because
 * it is pure arithmetic with a leap year rule in it, and a leap year
 * rule is exactly the kind of thing that is right for four years */
void ext2_unix_to_time(uint32_t seconds, struct fat32_time *out);
uint32_t ext2_time_to_unix(const struct fat32_time *t);

#endif
