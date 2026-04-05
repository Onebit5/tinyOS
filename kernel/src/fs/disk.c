#include "fs/disk.h"
#include "fs/fat32.h"
#include "drivers/ahci.h"
#include "lib/string.h"
#include "cpu/interrupts.h"
#include "sched/spinlock.h"
#include "drivers/rtc.h"

/* the filesystem keeps one sector of scratch and every path through
 * it assumes nobody else is halfway through another. that was a
 * promise of a race rather than a race, and this is it being kept */
static struct spinlock disk_lock = SPINLOCK("disk", LOCK_RANK_DEVICE);

static struct fat32 fs;
static bool ready;

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

bool disk_mount(void) {
    ready = false;
    if (!ahci_init()) {
        return false;
    }

    /* try each drive until one has a filesystem I recognise. the drive
     * this machine booted from is a disk like any other, and the one
     * with the files on it is not necessarily first -- so rather than
     * guess, ask each in turn. a drive with no boot sector of the right
     * shape simply fails to mount and the next one gets a go */
    for (size_t i = 0; i < ahci_disk_count(); i++) {
        if (!ahci_use_disk(i)) {
            continue;
        }
        if (fat32_mount(&fs, ahci_read, ahci_write, NULL)) {
            fat32_set_clock(&fs, disk_now);
            ready = true;
            return true;
        }
    }
    return false;
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

static void fill(struct disk_entry *out, const struct fat32_file *f) {
    size_t i = 0;
    while (f->name[i] != '\0' && i < DISK_NAME_MAX - 1) {
        out->name[i] = f->name[i];
        i++;
    }
    out->name[i] = '\0';
    out->size = f->size;
    out->cluster = f->first_cluster;
    out->is_dir = f->is_dir;
    out->entry_sector = f->entry_sector;
    out->entry_offset = f->entry_offset;
    out->written = f->written;
}


/* ---- the calls above me make --------------------------------------- */

bool disk_lookup(const char *path, struct disk_entry *out) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    struct fat32_file f;
    bool ok = fat32_lookup(&fs, below(path), &f);
    if (ok) {
        fill(out, &f);
    }
    leave(flags);
    return ok;
}

bool disk_readdir(const char *path, size_t index, struct disk_entry *out) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();

    struct fat32_file dir;
    bool ok = fat32_lookup(&fs, below(path), &dir) && dir.is_dir;
    if (ok) {
        struct fat32_file f;
        ok = fat32_readdir(&fs, dir.first_cluster, index, &f);
        if (ok) {
            fill(out, &f);
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

    /* a descriptor remembers where a file starts and how big it is,
     * which is all fat32_read needs to find any byte of it */
    struct fat32_file f;
    memset(&f, 0, sizeof f);
    f.first_cluster = cluster;
    f.size = (uint32_t)size;
    f.is_dir = false;

    int64_t n = fat32_read(&fs, &f, offset, buf, len);
    leave(flags);
    return n;
}

bool disk_create(const char *path, struct disk_entry *out) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    struct fat32_file f;
    bool ok = fat32_create(&fs, below(path), &f);
    if (ok) {
        fill(out, &f);
    }
    leave(flags);
    return ok;
}

int64_t disk_write_at(struct disk_entry *e, uint64_t offset, const void *buf,
                      uint64_t len) {
    if (!ready || e->is_dir || e->entry_sector == 0) {
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

    int64_t n = fat32_write(&fs, &f, offset, buf, len);
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
    bool ok = fat32_mkdir(&fs, below(path));
    leave(flags);
    return ok;
}

bool disk_rmdir(const char *path) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = fat32_rmdir(&fs, below(path));
    leave(flags);
    return ok;
}

bool disk_unlink(const char *path) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = fat32_unlink(&fs, below(path));
    leave(flags);
    return ok;
}

bool disk_rename(const char *from, const char *to) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = fat32_rename(&fs, below(from), below(to));
    leave(flags);
    return ok;
}

/* ---- what to say about it ------------------------------------------ */

const char *disk_label(void) { return ready ? fs.label : ""; }
const char *disk_model(void) { return ahci_model(); }

uint64_t disk_bytes(void) {
    return ahci_sectors() * AHCI_SECTOR;
}

uint32_t disk_cluster_bytes(void) {
    return ready ? fat32_cluster_bytes(&fs) : 0;
}

bool disk_usage(uint64_t *used_bytes, uint64_t *total_bytes) {
    if (!ready) {
        return false;
    }
    uint64_t flags = enter();
    uint32_t used, total;
    bool ok = fat32_usage(&fs, &used, &total);
    leave(flags);

    if (ok) {
        uint64_t per = fat32_cluster_bytes(&fs);
        *used_bytes = (uint64_t)used * per;
        *total_bytes = (uint64_t)total * per;
    }
    return ok;
}
