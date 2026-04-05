#ifndef FS_VFS_H
#define FS_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "fs/disk.h"

/* one namespace, two filesystems underneath it.
 *
 * until now the ramdisk was the whole world and the disk was bolted on
 * at /disk, which had it backwards. the disk is the bigger, writable,
 * persistent thing; the ramdisk is the small read-only one that is
 * always there. so:
 *
 *     /          the disk, when there is one
 *     /boot      the ramdisk, always
 *
 * the ramdisk keeps its own mount point rather than being deleted,
 * because it is what makes the machine work when the disk does not.
 * every program, and the passwd file, live on it. a kernel whose only
 * filesystem needs a sata controller to bring up is a kernel that a
 * missing cable turns into a brick -- and it is the obvious foundation
 * for booting from one medium to install onto another.
 *
 * a name with no leading slash is looked for on the disk first and the
 * ramdisk second. that is what lets a disk supply a newer `bin/ls`
 * while a machine with no disk carries on with the one it booted with,
 * and it is why nothing above this had to change to gain either. */

#define VFS_NAME_MAX 128
#define VFS_BOOT     "/boot"

enum vfs_kind {
    VFS_NOWHERE = 0,
    VFS_RAMDISK,
    VFS_DISK,
};

struct vfs_file {
    enum vfs_kind kind;

    char     name[VFS_NAME_MAX];
    uint64_t size;
    bool     is_dir;

    /* the tar header's mode, for the ramdisk. files on the disk have no
     * per-file permissions at all -- fat has never had any -- so they
     * take the mount's, which is "the master may write, anyone may
     * read" */
    uint32_t mode;

    /* where the bytes are: already in memory, or out on the disk */
    const void *data;
    uint32_t    cluster;
    uint64_t    entry_sector;
    uint32_t    entry_offset;

    /* when it was last written. files in the ramdisk have no answer to
     * that -- the tar has one, but every file in it was written by the
     * build, at once, which is a fact about the build and not about the
     * file -- so they come back as zero, which is fat's way of saying
     * nobody knows */
    struct fat32_time written;
};

/* resolve a path. absolute names go where they point; relative ones are
 * tried on the disk and then the ramdisk */
bool vfs_open(const char *path, struct vfs_file *out);

/* the nth thing in a directory. "/" lists the disk's root plus the
 * mount points standing in it; "/boot" lists the ramdisk */
bool vfs_readdir(const char *path, size_t index, struct vfs_file *out);

/* make a file. only the disk can, and it says so when it cannot */
bool vfs_create(const char *path, struct vfs_file *out);

/* and directories. the ramdisk is a tar in read-only memory, so `/boot`
 * refuses both -- there is nowhere for a new name to go */
bool vfs_mkdir(const char *path);
bool vfs_rmdir(const char *path);

/* remove a file. directories go through vfs_rmdir, which will not
 * remove one that still has anything in it */
bool vfs_unlink(const char *path);

/* rename, which is also how a file is moved: both are one name being
 * replaced by another, and neither copies a byte. within one mount
 * only -- a name cannot move from the ramdisk to the disk, because
 * that would be a copy wearing a rename's clothes */
bool vfs_rename(const char *from, const char *to);

int64_t vfs_read(const struct vfs_file *f, uint64_t offset, void *buf,
                 uint64_t len);
int64_t vfs_write(struct vfs_file *f, uint64_t offset, const void *buf,
                  uint64_t len);

/* may a process running as `uid` read this? */
bool vfs_may_read(const struct vfs_file *f, int uid);

/* may anyone write here at all? the ramdisk is read-only memory */
bool vfs_writable(const struct vfs_file *f);

/* the whole of a file in one piece, for the things that need it all at
 * once -- loading a program, reading passwd. a file already in memory
 * is handed over where it lies and `owned` comes back false; one on the
 * disk is read into the heap and the caller has to free it */
bool vfs_slurp(const char *path, const void **data, uint64_t *size,
               bool *owned);
void vfs_release(const void *data, bool owned);

/* the mount table, for `mount` to print */
struct vfs_mount {
    const char *at;
    const char *what;
    const char *where;
    bool        writable;
    bool        present;
};

size_t vfs_mount_count(void);
bool   vfs_mount_at(size_t index, struct vfs_mount *out);

#endif
