#include "fs/disk.h"
#include "fs/fat32.h"
#include "drivers/ahci.h"
#include "lib/string.h"
#include "arch/irq.h"
#include "sched/spinlock.h"
#include "drivers/rtc.h"
#include "fs/bcache.h"
#include "drivers/part.h"

/* the filesystem keeps one sector of scratch and every path through
 * it assumes nobody else is halfway through another. that was a
 * promise of a race rather than a race, and this is it being kept */
static struct spinlock disk_lock = SPINLOCK("disk", LOCK_RANK_DEVICE);

/* one of these is live at a time, and `kind` says which. two
 * filesystems behind one set of calls is what "the vfs has two things
 * to be a layer over" means in practice -- and the interesting part is
 * how much of what is above this did not have to change at all */
static struct fat32 fat;
static struct ext2 ext;
static enum disk_kind kind;
static bool ready;

/* ext2 keeps seconds since 1970; fat keeps a broken-down date. the
 * clock the machine has is broken-down, so this is the other direction */
static uint32_t ext2_now(void) {
    struct fat32_time t;
    struct rtc_time r;
    rtc_read(&r);
    t.year = r.year; t.month = r.month; t.day = r.day;
    t.hour = r.hour; t.minute = r.minute; t.second = r.second;
    return ext2_time_to_unix(&t);
}

/* the filesystem keeps one sector of scratch and every path through it
 * assumes nobody else is halfway through another. two threads reading
 * at once would hand each other the wrong sector, so they do not */
static uint64_t enter(void) { return spin_lock_irq(&disk_lock); }
static void leave(uint64_t flags) { spin_unlock_irq(&disk_lock, flags); }

/* what the filesystem asks when it needs to stamp something. the cmos
 * clock is the only thing on this machine that knows the date, and the
 * filesystem is not allowed to know that it exists -- so it gets handed
 * this instead */
static void disk_now(struct fat32_time *out) {
    struct rtc_time t;
    rtc_read(&t);
    out->year = t.year;
    out->month = t.month;
    out->day = t.day;
    out->hour = t.hour;
    out->minute = t.minute;
    out->second = t.second;
}

bool disk_ready(void) { return ready; }

enum disk_kind disk_which(void) { return ready ? kind : DISK_NONE; }

const char *disk_kind_name(void) {
    switch (disk_which()) {
    case DISK_EXT2:  return "ext2";
    case DISK_FAT32: return "fat32";
    default:         return "none";
    }
}

/* ---- partitions ------------------------------------------------------
 *
 * what a drive said it holds, for every drive, gathered at boot. the
 * filesystem is handed a view of *one* of these rather than of the
 * drive -- so every address it uses is relative to its own partition
 * and it never has to know it is not alone on the disk */

#define DISK_PARTS_MAX 16

static struct disk_part table[DISK_PARTS_MAX];
static size_t table_count;
static int mounted = -1;

/* the partition the filesystem is looking through. every read and write
 * it makes is shifted by this and bounded by it, which is the whole of
 * what a partition is */
static uint64_t view_first;
static uint64_t view_sectors;

static bool view_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    if (lba + count > view_sectors) {
        /* past the end of the partition, which is *not* the same as past
         * the end of the drive -- and letting one become the other is
         * how a filesystem writes over its neighbour */
        return false;
    }
    return bcache_read(ctx, view_first + lba, count, buf);
}

static bool view_write(void *ctx, uint64_t lba, uint32_t count,
                       const void *buf) {
    if (lba + count > view_sectors) {
        return false;
    }
    return bcache_write(ctx, view_first + lba, count, buf);
}

/* try to put a filesystem on one of them. ext2 first, because it is the
 * one that can answer every question */
static bool try_mount(size_t index) {
    struct disk_part *e = &table[index];
    if (!ahci_use_disk(e->p.drive)) {
        return false;
    }

    /* the cache holds absolute addresses and is therefore per drive, so
     * changing drives means starting it again -- and anything still
     * dirty in it belongs to the drive being left */
    bcache_sync();
    bcache_init(ahci_read, ahci_write, NULL);

    view_first = e->p.first_lba;
    view_sectors = e->p.sectors;

    /* the filesystem is handed the view rather than the drive. it was
     * already being handed a way to move sectors and it still is --
     * which is why neither ext2 nor fat32 needed a line changed */
    if (ext2_mount(&ext, view_read, view_write, NULL)) {
        ext2_set_clock(&ext, ext2_now);
        kind = DISK_EXT2;
        ready = true;
        mounted = (int)index;
        e->mountable = true;
        e->fs = "ext2";
        return true;
    }
    if (fat32_mount(&fat, view_read, view_write, NULL)) {
        fat32_set_clock(&fat, disk_now);
        kind = DISK_FAT32;
        ready = true;
        mounted = (int)index;
        e->mountable = true;
        e->fs = "fat32";
        return true;
    }
    return false;
}

bool disk_mount(void) {
    ready = false;
    kind = DISK_NONE;
    table_count = 0;
    mounted = -1;

    if (!ahci_init()) {
        return false;
    }

    /* every drive, and everything each of them says it holds. the drive
     * this machine booted from is a disk like any other and the one
     * with the files on it is not necessarily first, so rather than
     * guess, ask all of them and then decide */
    for (size_t i = 0; i < ahci_disk_count() && table_count < DISK_PARTS_MAX;
         i++) {
        if (!ahci_use_disk(i)) {
            continue;
        }

        struct partition found[PART_MAX];
        enum part_scheme scheme;
        size_t n = part_scan(ahci_read, NULL, found, PART_MAX, &scheme);

        if (n == 0) {
            /* no table. an image written straight to sector zero is a
             * perfectly ordinary thing, so it gets one entry covering
             * the whole drive rather than a special case everywhere
             * above this */
            struct disk_part *e = &table[table_count++];
            memset(e, 0, sizeof *e);
            e->p.drive = (unsigned)i;
            e->p.first_lba = 0;
            e->p.sectors = ahci_sectors();
            e->p.kind = "whole drive";
            e->scheme = PART_NONE;
            e->fs = "";
            continue;
        }

        for (size_t k = 0; k < n && table_count < DISK_PARTS_MAX; k++) {
            struct disk_part *e = &table[table_count++];
            memset(e, 0, sizeof *e);
            e->p = found[k];
            e->p.drive = (unsigned)i;
            e->scheme = scheme;
            e->fs = "";
        }
    }

    /* and now the first one with something on it. every partition is
     * tried rather than only the first -- an efi system partition in
     * slot one and the real filesystem in slot two is the ordinary
     * arrangement, not an unusual one */
    for (size_t i = 0; i < table_count; i++) {
        if (try_mount(i)) {
            return true;
        }
    }
    return false;
}



size_t disk_part_count(void) { return table_count; }

bool disk_part_at(size_t index, struct disk_part *out) {
    if (index >= table_count) {
        return false;
    }
    *out = table[index];
    return true;
}

int disk_mounted_part(void) { return mounted; }

bool disk_mount_part(size_t index) {
    if (index >= table_count) {
        return false;
    }
    uint64_t flags = enter();
    bool was_ready = ready;
    ready = false;              /* nothing may reach the old one now */
    bool ok = try_mount(index);
    if (!ok) {
        ready = was_ready;      /* put it back; nothing was disturbed */
    }
    leave(flags);
    return ok;
}

/* ---- paths --------------------------------------------------------- */

/* the disk is the root now, so a path arrives already rooted at it and
 * all that is left is the leading slash fat32 has no use for */
static const char *below(const char *path) {
    while (*path == '/') {
        path++;
    }
    return path;
}

static void copy_name(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] != '\0' && i < DISK_NAME_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void fill(struct disk_entry *out, const struct fat32_file *f) {
    memset(out, 0, sizeof *out);
    copy_name(out->name, f->name);
    out->size = f->size;
    out->cluster = f->first_cluster;
    out->is_dir = f->is_dir;
    out->entry_sector = f->entry_sector;
    out->entry_offset = f->entry_offset;
    out->written = f->written;

    /* fat records no ownership and no permissions. rather than invent
     * some per file, everything on it takes the mount's -- which is
     * what 0.1.11 decided and what 0.2.14 exists to stop having to */
    out->mode = 0644;
    out->uid = 0;
    out->gid = 0;
}

static void fill_ext2(struct disk_entry *out, const struct ext2_file *f) {
    memset(out, 0, sizeof *out);
    copy_name(out->name, f->name);
    out->size = f->size;
    out->is_dir = f->is_dir;
    out->is_symlink = f->is_symlink;
    out->written = f->modified;
    out->mode = f->mode & 0xfff;
    out->uid = f->uid;
    out->gid = f->gid;
    out->ino = f->ino;

    /* an ext2 file is named by its inode rather than by where its
     * directory entry happens to sit, so `cluster` carries the inode
     * and the two `entry_` fields have nothing to say */
    out->cluster = f->ino;
}


/* ---- the calls above me make --------------------------------------- */

bool disk_lookup(const char *path, struct disk_entry *out) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;
    if (kind == DISK_EXT2) {
        struct ext2_file f;
        ok = ext2_lookup(&ext, below(path), &f);
        if (ok) fill_ext2(out, &f);
    } else {
        struct fat32_file f;
        ok = fat32_lookup(&fat, below(path), &f);
        if (ok) fill(out, &f);
    }
    leave(flags);
    return ok;
}

bool disk_lookup_nofollow(const char *path, struct disk_entry *out) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;
    if (kind == DISK_EXT2) {
        struct ext2_file f;
        ok = ext2_lookup_nofollow(&ext, below(path), &f);
        if (ok) fill_ext2(out, &f);
    } else {
        /* fat has no symlinks, so there is nothing a lookup could
         * follow and this is the same question */
        struct fat32_file f;
        ok = fat32_lookup(&fat, below(path), &f);
        if (ok) fill(out, &f);
    }
    leave(flags);
    return ok;
}

bool disk_readlink(const char *path, char *out, size_t size) {
    if (!ready || kind != DISK_EXT2) {
        return false;
    }
    uint64_t flags = enter();
    struct ext2_file f;
    bool ok = ext2_lookup_nofollow(&ext, below(path), &f)
           && ext2_readlink(&ext, &f, out, size);
    leave(flags);
    return ok;
}

bool disk_readdir(const char *path, size_t index, struct disk_entry *out) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();

    bool ok;
    if (kind == DISK_EXT2) {
        struct ext2_file dir;
        ok = ext2_lookup(&ext, below(path), &dir) && dir.is_dir;
        if (ok) {
            struct ext2_file f;
            ok = ext2_readdir(&ext, dir.ino, index, &f);
            if (ok) fill_ext2(out, &f);
        }
    } else {
        struct fat32_file dir;
        ok = fat32_lookup(&fat, below(path), &dir) && dir.is_dir;
        if (ok) {
            struct fat32_file f;
            ok = fat32_readdir(&fat, dir.first_cluster, index, &f);
            if (ok) fill(out, &f);
        }
    }

    leave(flags);
    return ok;
}

int64_t disk_read(uint32_t cluster, uint64_t size, uint64_t offset,
                  void *buf, uint64_t len) {
    if (!ready) {
        return -1;
    }
    uint64_t flags = enter();

    int64_t n;
    if (kind == DISK_EXT2) {
        /* on ext2 a descriptor remembers the inode number, which is
         * the file's identity rather than a place on the disk */
        struct ext2_file f;
        memset(&f, 0, sizeof f);
        f.ino = cluster;
        f.size = size;
        n = ext2_read(&ext, &f, offset, buf, len);
    } else {
        /* a fat descriptor remembers where the file starts and how big
         * it is, which is all fat32_read needs to find any byte of it */
        struct fat32_file f;
        memset(&f, 0, sizeof f);
        f.first_cluster = cluster;
        f.size = (uint32_t)size;
        f.is_dir = false;
        n = fat32_read(&fat, &f, offset, buf, len);
    }
    leave(flags);
    return n;
}

bool disk_create(const char *path, struct disk_entry *out) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;
    if (kind == DISK_EXT2) {
        struct ext2_file f;
        ok = ext2_create(&ext, below(path), 0644, 0, 0, &f);
        if (ok) fill_ext2(out, &f);
    } else {
        struct fat32_file f;
        ok = fat32_create(&fat, below(path), &f);
        if (ok) fill(out, &f);
    }
    leave(flags);
    return ok;
}

int64_t disk_write_at(struct disk_entry *e, uint64_t offset, const void *buf,
                      uint64_t len) {
    if (!ready || e->is_dir) {
        return -1;
    }
    if (kind == DISK_EXT2) {
        uint64_t flags = enter();
        struct ext2_file f;
        memset(&f, 0, sizeof f);
        f.ino = e->cluster;
        f.size = e->size;
        int64_t n = ext2_write(&ext, &f, offset, buf, len);
        if (n > 0) {
            e->size = f.size;
        }
        leave(flags);
        return n;
    }
    if (e->entry_sector == 0) {
        return -1;
    }
    uint64_t flags = enter();

    /* rebuild what the filesystem wants out of what the descriptor
     * remembered. the record's address is the part that matters -- it
     * is what lets the new size be written back where it belongs */
    struct fat32_file f;
    memset(&f, 0, sizeof f);
    f.first_cluster = e->cluster;
    f.size = (uint32_t)e->size;
    f.is_dir = false;
    f.attr = FAT32_ATTR_ARCHIVE;
    f.entry_sector = e->entry_sector;
    f.entry_offset = e->entry_offset;

    int64_t n = fat32_write(&fat, &f, offset, buf, len);
    if (n > 0) {
        e->size = f.size;
        e->cluster = f.first_cluster;
    }

    leave(flags);
    return n;
}

bool disk_mkdir(const char *path) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (kind == DISK_EXT2) ? ext2_mkdir(&ext, below(path), 0755, 0, 0)
                                  : fat32_mkdir(&fat, below(path));
    leave(flags);
    return ok;
}

bool disk_rmdir(const char *path) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (kind == DISK_EXT2) ? ext2_rmdir(&ext, below(path))
                                  : fat32_rmdir(&fat, below(path));
    leave(flags);
    return ok;
}

bool disk_unlink(const char *path) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (kind == DISK_EXT2) ? ext2_unlink(&ext, below(path))
                                  : fat32_unlink(&fat, below(path));
    leave(flags);
    return ok;
}

bool disk_rename(const char *from, const char *to) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (kind == DISK_EXT2)
            ? ext2_rename(&ext, below(from), below(to))
            : fat32_rename(&fat, below(from), below(to));
    leave(flags);
    return ok;
}

/* ---- the things fat had nowhere to write down ----------------------
 *
 * all three answer false on a fat disk, and say so rather than
 * pretending. a filesystem that cannot record an owner cannot be given
 * one, and a chmod that silently did nothing would be worse than a
 * chmod that refuses */
bool disk_chmod(const char *path, uint32_t mode) {
    if (!ready || kind != DISK_EXT2) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = ext2_chmod(&ext, below(path), mode);
    leave(flags);
    return ok;
}

bool disk_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!ready || kind != DISK_EXT2) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = ext2_chown(&ext, below(path), uid, gid);
    leave(flags);
    return ok;
}

bool disk_symlink(const char *path, const char *target) {
    if (!ready || kind != DISK_EXT2) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = ext2_symlink(&ext, below(path), target, 0, 0);
    leave(flags);
    return ok;
}

/* everything the cache is holding, onto the drive.
 *
 * takes the disk lock like everything else here, because the cache has
 * none of its own -- it is protected by this one, and a sync arriving
 * from a timer or from somebody typing `sync` is the only path into it
 * that does not already come through a filesystem call */
bool disk_sync(void) {
    if (!ready) {
        return true;    /* nothing to lose */
    }
    uint64_t flags = enter();
    bool ok = bcache_sync();
    leave(flags);
    return ok;
}

bool disk_dirty(void) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool any = bcache_dirty();
    leave(flags);
    return any;
}

void disk_cache_stats(struct bcache_stats *out) {
    memset(out, 0, sizeof *out);
    if (!ready) {
        return;
    }
    uint64_t flags = enter();
    bcache_get_stats(out);
    leave(flags);
}

/* ---- what to say about it ------------------------------------------ */

const char *disk_label(void) {
    if (!ready) {
        return "";
    }
    return (kind == DISK_EXT2) ? ext.label : fat.label;
}

const char *disk_model(void) { return ahci_model(); }

uint64_t disk_bytes(void) {
    return ahci_sectors() * AHCI_SECTOR;
}

/* ext2 calls them blocks and fat calls them clusters. they are the same
 * idea -- the smallest thing the filesystem allocates -- so this is one
 * question with two names for it */
uint32_t disk_cluster_bytes(void) {
    if (!ready) {
        return 0;
    }
    return (kind == DISK_EXT2) ? ext2_block_bytes(&ext)
                               : fat32_cluster_bytes(&fat);
}

bool disk_usage(uint64_t *used_bytes, uint64_t *total_bytes) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;

    if (kind == DISK_EXT2) {
        ok = ext2_usage(&ext, used_bytes, total_bytes);
    } else {
        uint32_t used, total;
        ok = fat32_usage(&fat, &used, &total);
        if (ok) {
            uint64_t per = fat32_cluster_bytes(&fat);
            *used_bytes = (uint64_t)used * per;
            *total_bytes = (uint64_t)total * per;
        }
    }

    leave(flags);
    return ok;
}
