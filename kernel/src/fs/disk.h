#ifndef FS_DISK_H
#define FS_DISK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "fs/fat32.h"

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
};

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

/* what to tell the user about it */
const char *disk_label(void);
const char *disk_model(void);
uint64_t    disk_bytes(void);
bool        disk_usage(uint64_t *used_bytes, uint64_t *total_bytes);
uint32_t    disk_cluster_bytes(void);

#endif
