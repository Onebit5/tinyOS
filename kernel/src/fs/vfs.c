#include "fs/vfs.h"
#include "fs/ramdisk.h"
#include "fs/disk.h"
#include "mm/kmalloc.h"
#include "lib/string.h"

/* what a file on the disk is allowed to be. fat records no ownership
 * and no permissions, so rather than invent some, everything on it
 * takes the mount's: readable by anyone, writable by the master */
#define DISK_MODE 0644

static void copy_name(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] != '\0' && i < VFS_NAME_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---- which mount does this name belong to -------------------------- */

/* "/boot", "/boot/", "/boot/anything" -- but not "/bootleg" */
static bool under_boot(const char *path, const char **rest) {
    size_t n = strlen(VFS_BOOT);
    for (size_t i = 0; i < n; i++) {
        if (path[i] != VFS_BOOT[i]) {
            return false;
        }
    }
    if (path[n] != '\0' && path[n] != '/') {
        return false;
    }

    const char *tail = path + n;
    while (*tail == '/') {
        tail++;
    }
    *rest = tail;
    return true;
}

static void from_ramdisk(struct vfs_file *out, const struct ramdisk_file *f) {
    memset(out, 0, sizeof *out);
    out->kind = VFS_RAMDISK;

    const char *name = f->name;
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
    }
    copy_name(out->name, name);

    out->size = f->size;
    out->mode = f->mode;
    out->data = f->data;

    size_t n = strlen(out->name);
    out->is_dir = (n > 0 && out->name[n - 1] == '/');
}

static void from_disk(struct vfs_file *out, const struct disk_entry *e) {
    memset(out, 0, sizeof *out);
    out->kind = VFS_DISK;
    copy_name(out->name, e->name);
    out->size = e->size;
    out->is_dir = e->is_dir;
    out->mode = DISK_MODE;
    out->cluster = e->cluster;
    out->entry_sector = e->entry_sector;
    out->entry_offset = e->entry_offset;
}

/* the disk's copy of a name, if there is a disk and it has one */
static bool try_disk(const char *path, struct vfs_file *out) {
    if (!disk_ready()) {
        return false;
    }
    struct disk_entry e;
    if (!disk_lookup(path, &e)) {
        return false;
    }
    from_disk(out, &e);
    return true;
}

static bool try_ramdisk(const char *path, struct vfs_file *out) {
    struct ramdisk_file f;
    if (!ramdisk_open(path, &f)) {
        return false;
    }
    from_ramdisk(out, &f);
    return true;
}

bool vfs_open(const char *path, struct vfs_file *out) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    const char *rest;
    if (under_boot(path, &rest)) {
        return try_ramdisk(rest, out);
    }

    if (path[0] == '/') {
        return try_disk(path, out);
    }

    /* a bare name: the disk first, so a disk can supply a newer copy of
     * something, then the ramdisk, so a machine without one still finds
     * everything it booted with */
    char absolute[VFS_NAME_MAX + 1];
    absolute[0] = '/';
    copy_name(absolute + 1, path);
    if (try_disk(absolute, out)) {
        return true;
    }
    return try_ramdisk(path, out);
}

/* ---- listing ------------------------------------------------------- */

/* "/" has the disk's root in it and also the mount points standing on
 * it, which belong to no filesystem and have to be added by hand */
static bool root_extra(size_t index, struct vfs_file *out) {
    if (index != 0) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->kind = VFS_RAMDISK;
    copy_name(out->name, VFS_BOOT + 1);     /* past the slash */
    out->is_dir = true;
    out->mode = 0555;
    return true;
}

bool vfs_readdir(const char *path, size_t index, struct vfs_file *out) {
    if (path == NULL || path[0] == '\0') {
        path = "/";
    }

    /* a relative directory is relative to the root, the same as a
     * relative filename is. `ls boot/` has to mean what `ls /boot`
     * means -- vfs_open took relative names from the start and this
     * did not, which is exactly the sort of difference nobody expects */
    char absolute[VFS_NAME_MAX + 1];
    if (path[0] != '/') {
        absolute[0] = '/';
        copy_name(absolute + 1, path);
        path = absolute;
    }

    const char *rest;
    if (under_boot(path, &rest)) {
        /* the ramdisk is flat: an index and a name is the whole of it,
         * and there is nothing to descend into */
        if (rest[0] != '\0') {
            return false;
        }
        struct ramdisk_file f;
        size_t seen = 0;
        for (size_t i = 0; ramdisk_stat(i, &f); i++) {
            struct vfs_file v;
            from_ramdisk(&v, &f);
            if (v.name[0] == '\0' || v.is_dir) {
                continue;       /* tar's directory records lead nowhere */
            }
            if (seen++ == index) {
                *out = v;
                return true;
            }
        }
        return false;
    }

    /* "/" and "//" and "/////" are all the root, and a trailing slash
     * never changes which directory is meant */
    bool is_root = true;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p != '/') {
            is_root = false;
            break;
        }
    }

    if (disk_ready()) {
        struct disk_entry e;
        if (disk_readdir(path, index, &e)) {
            from_disk(out, &e);
            return true;
        }
        /* past the end of the disk's root, the mounts standing on it */
        if (is_root) {
            size_t count = 0;
            struct disk_entry ignored;
            while (disk_readdir(path, count, &ignored)) {
                count++;
            }
            return root_extra(index - count, out);
        }
        return false;
    }

    /* no disk at all: the root holds nothing but the mount points */
    return is_root && root_extra(index, out);
}

/* ---- the rest ------------------------------------------------------ */

bool vfs_create(const char *path, struct vfs_file *out) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    const char *rest;
    if (under_boot(path, &rest)) {
        return false;       /* read-only memory. there is nowhere to put it */
    }
    if (!disk_ready()) {
        return false;
    }

    char absolute[VFS_NAME_MAX + 1];
    if (path[0] != '/') {
        absolute[0] = '/';
        copy_name(absolute + 1, path);
        path = absolute;
    }

    struct disk_entry e;
    if (!disk_create(path, &e)) {
        return false;
    }
    from_disk(out, &e);
    return true;
}

int64_t vfs_read(const struct vfs_file *f, uint64_t offset, void *buf,
                 uint64_t len) {
    if (f->is_dir) {
        return -1;
    }
    if (offset >= f->size) {
        return 0;
    }
    if (offset + len > f->size) {
        len = f->size - offset;
    }

    if (f->kind == VFS_RAMDISK) {
        memcpy(buf, (const uint8_t *)f->data + offset, len);
        return (int64_t)len;
    }
    if (f->kind == VFS_DISK) {
        return disk_read(f->cluster, f->size, offset, buf, len);
    }
    return -1;
}

int64_t vfs_write(struct vfs_file *f, uint64_t offset, const void *buf,
                  uint64_t len) {
    if (f->kind != VFS_DISK || f->is_dir) {
        return -1;
    }

    struct disk_entry e;
    memset(&e, 0, sizeof e);
    e.size = f->size;
    e.cluster = f->cluster;
    e.is_dir = false;
    e.entry_sector = f->entry_sector;
    e.entry_offset = f->entry_offset;

    int64_t n = disk_write_at(&e, offset, buf, len);
    if (n > 0) {
        f->size = e.size;
        f->cluster = e.cluster;
    }
    return n;
}

bool vfs_may_read(const struct vfs_file *f, int uid) {
    return uid == 0 || (f->mode & 0004) != 0;
}

bool vfs_writable(const struct vfs_file *f) {
    return f->kind == VFS_DISK;
}

/* ---- whole files --------------------------------------------------- */

bool vfs_slurp(const char *path, const void **data, uint64_t *size,
               bool *owned) {
    struct vfs_file f;
    if (!vfs_open(path, &f) || f.is_dir) {
        return false;
    }

    /* already in memory, so hand it over where it lies. this is why
     * loading a program off the ramdisk costs nothing at all */
    if (f.kind == VFS_RAMDISK) {
        *data = f.data;
        *size = f.size;
        *owned = false;
        return true;
    }

    if (f.size == 0) {
        return false;
    }
    void *buf = kmalloc(f.size);
    if (buf == NULL) {
        return false;
    }
    if (vfs_read(&f, 0, buf, f.size) != (int64_t)f.size) {
        kfree(buf);
        return false;
    }

    *data = buf;
    *size = f.size;
    *owned = true;
    return true;
}

void vfs_release(const void *data, bool owned) {
    if (owned) {
        kfree((void *)data);
    }
}

/* ---- the mount table ----------------------------------------------- */

size_t vfs_mount_count(void) {
    return 2;
}

bool vfs_mount_at(size_t index, struct vfs_mount *out) {
    if (index == 0) {
        out->at = "/";
        out->what = "fat32";
        out->where = disk_ready() ? disk_model() : "nothing -- no disk found";
        out->writable = true;
        out->present = disk_ready();
        return true;
    }
    if (index == 1) {
        out->at = VFS_BOOT;
        out->what = "ustar";
        out->where = "a module the bootloader handed me";
        out->writable = false;
        out->present = ramdisk_present();
        return true;
    }
    return false;
}
