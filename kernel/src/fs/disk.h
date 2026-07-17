#ifndef FS_DISK_H
#define FS_DISK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "fs/fat32.h"
#include "fs/ext2.h"
#include "fs/bcache.h"
#include "drivers/part.h"

/* the disk, mounted.
 *
 * one place that owns the sata controller and the filesystem on it, so
 * that nothing above has to know which is which. paths are absolute and
 * rooted at the disk itself; deciding that a name belongs here at all
 * is the vfs's job, not this file's.
 *
 * everything degrades quietly. a machine with no disk, or a disk with
 * no filesystem I recognise, answers false to all of this and boots
 * exactly as it did before there was any of it. */

#define DISK_NAME_MAX   128

struct disk_entry {
    char     name[DISK_NAME_MAX];
    uint64_t size;
    uint32_t cluster;
    bool     is_dir;

    /* where the 32-byte record describing this file sits on the disk.
     * a write that grows a file has to go back and correct its size,
     * and this is how it finds the place -- the same job an inode
     * number does on a filesystem that has them */
    uint64_t entry_sector;
    uint32_t entry_offset;

    /* when it was last written. this is fat's struct rather than one of
     * my own on purpose: the fields are identical, and a second copy of
     * six integers would only mean writing a function that converts
     * between two things that are the same */
    struct fat32_time written;

    /* ---- what ext2 can say and fat cannot ----
     *
     * on a fat disk these are the mount's answer rather than the
     * file's, because fat has nowhere to keep them and inventing a
     * per-file answer would be a lie with a number in it. on an ext2
     * one they come out of the inode, which is the entire reason 0.2.14
     * happened */
    uint32_t mode;              /* permissions, without the type bits */
    uint32_t uid, gid;
    bool     is_symlink;
    uint32_t ino;               /* the identity. 0 where there is none */
};

/* which filesystem answered. a disk is not a filesystem and the vfs is
 * finally a layer over more than one of them */
enum disk_kind {
    DISK_NONE = 0,
    DISK_FAT32,
    DISK_EXT2,
};

enum disk_kind disk_which(void);
const char *disk_kind_name(void);

/* ---- partitions ------------------------------------------------------
 *
 * a disk is not a filesystem, so "which disk" was never the right
 * question. every drive is scanned at boot and what is found is kept
 * here -- including drives with no table at all, which get one entry
 * covering the whole of themselves, because an image written straight
 * to sector zero is a perfectly ordinary thing and should not need a
 * special case anywhere above this */

struct disk_part {
    struct partition p;
    enum part_scheme scheme;    /* how it was found, or PART_NONE for a
                                 * whole drive with no table */
    bool     mountable;         /* something recognised a filesystem on it */
    const char *fs;             /* what that was, or "" */
};

size_t disk_part_count(void);
bool   disk_part_at(size_t index, struct disk_part *out);

/* which one is mounted at /, or -1 */
int disk_mounted_part(void);

/* mount a particular one instead of whichever answered first. anything
 * open on the old one is stale afterwards, so this syncs first and the
 * shell warns -- there is no reference counting here to do better */
bool disk_mount_part(size_t index);

/* find a controller, mount what is on it. safe to call when there is
 * neither */
bool disk_mount(void);

bool disk_ready(void);

/* look a file up. `path` includes the prefix */
bool disk_lookup(const char *path, struct disk_entry *out);

/* the nth thing in a directory. `path` includes the prefix; "/disk" is
 * the root */
bool disk_readdir(const char *path, size_t index, struct disk_entry *out);

/* read from a file already found. cluster and size come from a lookup,
 * which is what a descriptor remembers instead of a pointer */
int64_t disk_read(uint32_t cluster, uint64_t size, uint64_t offset,
                  void *buf, uint64_t len);

/* make a file if it is not there, and hand back where it lives. the
 * name has to fit 8.3, since I mint short entries only */
bool disk_create(const char *path, struct disk_entry *out);

/* write to a file found by disk_create or disk_lookup. `e` is updated
 * in place: a file that grew has a new size, and one that was empty has
 * a first cluster it did not have before */
int64_t disk_write_at(struct disk_entry *e, uint64_t offset, const void *buf,
                      uint64_t len);

/* directories, made and unmade. the same 8.3 rule as disk_create */
bool disk_mkdir(const char *path);
bool disk_rmdir(const char *path);

/* remove a file, and give its clusters back. directories go through
 * disk_rmdir instead, which checks that they are empty first */
bool disk_unlink(const char *path);

/* give a file another name, possibly in another directory. nothing is
 * copied -- a rename moves a name, not a file */
bool disk_rename(const char *from, const char *to);

/* ---- the things fat had nowhere to write down -----------------------
 *
 * all of these answer false on a fat disk, and say so rather than
 * pretending to have worked. a filesystem that cannot record an owner
 * cannot be given one */
bool disk_chmod(const char *path, uint32_t mode);
bool disk_chown(const char *path, uint32_t uid, uint32_t gid);
bool disk_symlink(const char *path, const char *target);

/* where a symlink points, and looking one up *without* following it --
 * which is what `ls -l` and `rm` want */
bool disk_readlink(const char *path, char *out, size_t size);
bool disk_lookup_nofollow(const char *path, struct disk_entry *out);

/* ---- the cache ------------------------------------------------------
 *
 * writes do not reach the drive when they are made. they sit in memory
 * until the block is evicted, the flusher comes round, or somebody says
 * `sync` -- so between a write and one of those, the disk does not hold
 * what the machine says it holds. that is the trade, and this is the
 * half of it that puts things right */
bool disk_sync(void);

/* is there anything to lose? cheap enough to ask on a timer */
bool disk_dirty(void);

void disk_cache_stats(struct bcache_stats *out);

/* what to tell the user about it */
const char *disk_label(void);
const char *disk_model(void);
uint64_t    disk_bytes(void);
bool        disk_usage(uint64_t *used_bytes, uint64_t *total_bytes);
uint32_t    disk_cluster_bytes(void);

#endif
