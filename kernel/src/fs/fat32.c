#include "fs/fat32.h"
#include "lib/string.h"

#define BAD_CLUSTER  0x0ffffff7u
#define EOC          0x0ffffff8u     /* anything at or above ends a chain */
#define CLUSTER_MASK 0x0fffffffu     /* the top four bits are not mine */

/* ---- little endian, read a byte at a time -------------------------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- the disk ------------------------------------------------------ */

static bool read_sector(struct fat32 *fs, uint64_t lba, void *buf) {
    return fs->read(fs->ctx, lba, 1, buf);
}

static bool write_sector(struct fat32 *fs, uint64_t lba, const void *buf) {
    return fs->write != NULL && fs->write(fs->ctx, lba, 1, buf);
}

uint32_t fat32_cluster_bytes(const struct fat32 *fs) {
    return fs->sectors_per_cluster * FAT32_SECTOR;
}

static uint64_t cluster_lba(const struct fat32 *fs, uint32_t cluster) {
    return fs->first_data_sector
         + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static bool cluster_ok(const struct fat32 *fs, uint32_t cluster) {
    return cluster >= 2 && cluster < fs->cluster_count + 2;
}

/* ---- the table the whole thing is named after ---------------------- */

/* entry N says which cluster follows N. that is the entire idea */
static bool fat_get(struct fat32 *fs, uint32_t cluster, uint32_t *out) {
    if (!cluster_ok(fs, cluster)) {
        return false;
    }
    uint64_t offset = (uint64_t)cluster * 4;
    uint64_t lba = fs->reserved_sectors + offset / FAT32_SECTOR;

    if (!read_sector(fs, lba, fs->scratch)) {
        return false;
    }
    *out = rd32(&fs->scratch[offset % FAT32_SECTOR]) & CLUSTER_MASK;
    return true;
}

/* written to every copy of the table. the second copy exists precisely
 * so a machine that dies mid-write has something to fall back on, and
 * updating only one would defeat that */
static bool fat_set(struct fat32 *fs, uint32_t cluster, uint32_t value) {
    if (!cluster_ok(fs, cluster) || fs->write == NULL) {
        return false;
    }
    uint64_t offset = (uint64_t)cluster * 4;
    uint64_t within = offset / FAT32_SECTOR;

    for (uint32_t copy = 0; copy < fs->num_fats; copy++) {
        uint64_t lba = fs->reserved_sectors + copy * fs->fat_sectors + within;
        if (!read_sector(fs, lba, fs->scratch)) {
            return false;
        }
        uint8_t *slot = &fs->scratch[offset % FAT32_SECTOR];
        /* the top four bits belong to whoever formatted the disk */
        wr32(slot, (rd32(slot) & ~CLUSTER_MASK) | (value & CLUSTER_MASK));
        if (!write_sector(fs, lba, fs->scratch)) {
            return false;
        }
    }
    return true;
}

static uint32_t alloc_cluster(struct fat32 *fs) {
    if (fs->write == NULL) {
        return 0;
    }
    /* a linear search for a zero entry. the fsinfo sector carries a hint
     * about where to start looking, which I do not trust and do not
     * use -- it is advisory, and wrong on any disk that was not
     * unmounted cleanly */
    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        uint32_t value;
        if (!fat_get(fs, c, &value)) {
            return 0;
        }
        if (value == 0) {
            if (!fat_set(fs, c, EOC | 0x7)) {   /* 0x0fffffff, end of chain */
                return 0;
            }
            return c;
        }
    }
    return 0;       /* the disk is full */
}

/* ---- mounting ------------------------------------------------------ */

bool fat32_mount(struct fat32 *fs, fat32_io read, fat32_out write, void *ctx) {
    memset(fs, 0, sizeof *fs);
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;

    uint8_t boot[FAT32_SECTOR];
    if (read == NULL || !read(ctx, 0, 1, boot)) {
        return false;
    }

    /* the signature every boot sector ends with. its absence means this
     * is not a filesystem, or not one I know */
    if (boot[510] != 0x55 || boot[511] != 0xaa) {
        return false;
    }
    if (rd16(&boot[11]) != FAT32_SECTOR) {
        return false;       /* 512-byte sectors only, which is everything */
    }

    fs->sectors_per_cluster = boot[13];
    fs->reserved_sectors    = rd16(&boot[14]);
    fs->num_fats            = boot[16];
    fs->fat_sectors         = rd32(&boot[36]);
    fs->root_cluster        = rd32(&boot[44]);

    fs->total_sectors = rd16(&boot[19]);
    if (fs->total_sectors == 0) {
        fs->total_sectors = rd32(&boot[32]);
    }

    /* fat12 and fat16 put a number here and fat32 puts zero, which is
     * the cleanest way to tell them apart */
    if (rd16(&boot[22]) != 0 || fs->fat_sectors == 0) {
        return false;
    }
    if (fs->sectors_per_cluster == 0 || fs->num_fats == 0
        || fs->reserved_sectors == 0 || fs->root_cluster < 2) {
        return false;
    }

    fs->first_data_sector = fs->reserved_sectors
                          + (uint64_t)fs->num_fats * fs->fat_sectors;
    if (fs->total_sectors <= fs->first_data_sector) {
        return false;
    }
    fs->cluster_count = (uint32_t)((fs->total_sectors - fs->first_data_sector)
                                   / fs->sectors_per_cluster);

    /* the geometry may imply more clusters than the table has room to
     * describe. trusting it would mean reading the chain of a cluster
     * whose entry lives past the end of the fat -- which is to say, in
     * the next copy of the fat. believe the smaller of the two */
    uint64_t addressable = (uint64_t)fs->fat_sectors * FAT32_SECTOR / 4;
    if (addressable < 2 || fs->cluster_count > addressable - 2) {
        fs->cluster_count = (uint32_t)(addressable > 2 ? addressable - 2 : 0);
    }
    if (fs->cluster_count < 2) {
        return false;
    }

    /* the label, which lives in two places; the boot sector's copy is
     * the one that is always there */
    for (int i = 0; i < 11; i++) {
        fs->label[i] = (char)boot[71 + i];
    }
    fs->label[11] = '\0';
    for (int i = 10; i >= 0 && fs->label[i] == ' '; i--) {
        fs->label[i] = '\0';
    }

    fs->mounted = true;
    return true;
}

/* ---- names --------------------------------------------------------- */

/* "HELLO   TXT" -> "hello.txt". the two case bits are a later addition
 * that everybody implements: they say the base or the extension was
 * lowercase, which 8.3 has no other way of recording */
static void short_name(const uint8_t *entry, char *out) {
    bool lower_base = (entry[12] & 0x08) != 0;
    bool lower_ext  = (entry[12] & 0x10) != 0;
    size_t n = 0;

    for (int i = 0; i < 8 && entry[i] != ' '; i++) {
        char c = (char)entry[i];
        out[n++] = (lower_base && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (entry[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && entry[i] != ' '; i++) {
            char c = (char)entry[i];
            out[n++] = (lower_ext && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    out[n] = '\0';
}

/* the kernel's string lib has no strncpy and does not need one for the
 * sake of two call sites */
static void copy_name(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] != '\0' && i < FAT32_NAME_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint8_t short_checksum(const uint8_t *name11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)((((sum & 1) << 7) | (sum >> 1)) + name11[i]);
    }
    return sum;
}

/* a long name is spread over the entries *before* the short one, in
 * reverse, thirteen utf-16 characters at a time, in three runs at odd
 * offsets because those were the only bytes left unused */
static void lfn_chars(const uint8_t *entry, uint16_t *out) {
    static const int at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int i = 0; i < 13; i++) {
        out[i] = rd16(&entry[at[i]]);
    }
}

/* the state of a long name being collected across several entries */
struct lfn_state {
    char     name[FAT32_NAME_MAX];
    bool     valid;
    uint8_t  checksum;
    uint32_t want;      /* how many pieces the last entry said there were */
    uint32_t got;       /* one bit per piece that actually turned up */
};

static void lfn_reset(struct lfn_state *l) {
    l->valid = false;
    l->want = 0;
    l->got = 0;
    l->name[0] = '\0';
}

static void lfn_take(struct lfn_state *l, const uint8_t *entry) {
    uint8_t sequence = entry[0];
    bool last = (sequence & 0x40) != 0;
    uint32_t index = (uint32_t)(sequence & 0x1f);

    if (index == 0 || index > 10) {
        lfn_reset(l);
        return;
    }
    if (last) {
        lfn_reset(l);
        l->checksum = entry[13];
        l->valid = true;
        l->want = index;
        memset(l->name, 0, sizeof l->name);
    } else if (!l->valid || entry[13] != l->checksum) {
        lfn_reset(l);       /* a piece of some other name, or of nothing */
        return;
    }

    l->got |= 1u << (index - 1);

    uint16_t chars[13];
    lfn_chars(entry, chars);

    size_t base = (index - 1) * 13;
    for (int i = 0; i < 13; i++) {
        size_t at = base + (size_t)i;
        if (at >= FAT32_NAME_MAX - 1) {
            break;
        }
        uint16_t c = chars[i];
        if (c == 0x0000 || c == 0xffff) {
            continue;       /* padding past the end of the name */
        }
        /* anything outside ascii becomes a question mark rather than
         * half a character. I have no business pretending to unicode */
        l->name[at] = (c < 0x80) ? (char)c : '?';
    }
}

/* did every piece arrive, and does it belong to this entry? a long name
 * with a hole in it is worse than no long name at all */
static bool lfn_finish(struct lfn_state *l, const uint8_t *entry, char *out) {
    if (!l->valid || l->checksum != short_checksum(entry)) {
        return false;
    }
    /* every piece, not just some of them. a name assembled out of an
     * incomplete set would be silently truncated at the first gap,
     * which is a far worse answer than falling back to the short name */
    if (l->want == 0 || l->got != (1u << l->want) - 1) {
        return false;
    }
    if (l->name[0] == '\0') {
        return false;
    }
    copy_name(out, l->name);
    return true;
}

/* ---- walking a directory ------------------------------------------- */

/* directories are files. so walking one is walking a cluster chain and
 * reading 32-byte records out of it, and the only thing that makes the
 * root special is that its first cluster is written in the boot sector */
struct dir_walk {
    uint32_t cluster;
    uint32_t sector_in_cluster;
    uint32_t offset_in_sector;
    uint64_t lba;
    bool     done;
    struct lfn_state lfn;
};

static void walk_start(struct dir_walk *w, uint32_t cluster) {
    w->cluster = cluster;
    w->sector_in_cluster = 0;
    w->offset_in_sector = 0;
    w->done = false;
    lfn_reset(&w->lfn);
}

/* the next real entry, with its long name assembled if it had one.
 * returns false at the end of the directory */
static bool walk_next(struct fat32 *fs, struct dir_walk *w,
                      struct fat32_file *out) {
    while (!w->done) {
        if (!cluster_ok(fs, w->cluster)) {
            return false;
        }

        w->lba = cluster_lba(fs, w->cluster) + w->sector_in_cluster;
        if (!read_sector(fs, w->lba, fs->scratch)) {
            return false;
        }

        while (w->offset_in_sector < FAT32_SECTOR) {
            uint8_t *entry = &fs->scratch[w->offset_in_sector];
            uint64_t entry_lba = w->lba;
            uint32_t entry_off = w->offset_in_sector;
            w->offset_in_sector += 32;

            if (entry[0] == 0x00) {
                w->done = true;         /* nothing beyond here, ever */
                return false;
            }
            if (entry[0] == 0xe5) {
                lfn_reset(&w->lfn);     /* deleted */
                continue;
            }

            uint8_t attr = entry[11];
            if ((attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                lfn_take(&w->lfn, entry);
                continue;
            }
            if (attr & FAT32_ATTR_VOLUME_ID) {
                lfn_reset(&w->lfn);     /* the label, not a file */
                continue;
            }

            memset(out, 0, sizeof *out);
            if (!lfn_finish(&w->lfn, entry, out->name)) {
                short_name(entry, out->name);
            }
            lfn_reset(&w->lfn);

            out->attr = attr;
            out->is_dir = (attr & FAT32_ATTR_DIRECTORY) != 0;
            out->size = rd32(&entry[28]);
            out->first_cluster = ((uint32_t)rd16(&entry[20]) << 16)
                               | rd16(&entry[26]);
            out->entry_sector = entry_lba;
            out->entry_offset = entry_off;
            return true;
        }

        /* on to the next sector, and then the next cluster */
        w->offset_in_sector = 0;
        w->sector_in_cluster++;
        if (w->sector_in_cluster >= fs->sectors_per_cluster) {
            w->sector_in_cluster = 0;
            uint32_t next;
            if (!fat_get(fs, w->cluster, &next) || next >= EOC) {
                w->done = true;
                return false;
            }
            w->cluster = next;
        }
    }
    return false;
}

static bool is_dot(const char *name) {
    return name[0] == '.' && (name[1] == '\0'
                              || (name[1] == '.' && name[2] == '\0'));
}

bool fat32_readdir(struct fat32 *fs, uint32_t dir_cluster, size_t index,
                   struct fat32_file *out) {
    if (!fs->mounted) {
        return false;
    }
    if (dir_cluster == 0) {
        dir_cluster = fs->root_cluster;
    }

    struct dir_walk w;
    walk_start(&w, dir_cluster);

    size_t seen = 0;
    struct fat32_file f;
    while (walk_next(fs, &w, &f)) {
        if (is_dot(f.name)) {
            continue;       /* nothing above me has any use for these */
        }
        if (seen == index) {
            *out = f;
            return true;
        }
        seen++;
    }
    return false;
}

/* ---- looking a path up --------------------------------------------- */

static bool name_eq(const char *a, const char *b) {
    /* fat has never cared about case and neither do I */
    while (*a != '\0' && *b != '\0') {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) {
            return false;
        }
        a++; b++;
    }
    return *a == *b;
}

static bool find_in(struct fat32 *fs, uint32_t dir_cluster, const char *name,
                    struct fat32_file *out) {
    struct dir_walk w;
    walk_start(&w, dir_cluster);

    struct fat32_file f;
    while (walk_next(fs, &w, &f)) {
        if (name_eq(f.name, name)) {
            *out = f;
            return true;
        }
    }
    return false;
}

bool fat32_lookup(struct fat32 *fs, const char *path, struct fat32_file *out) {
    if (!fs->mounted) {
        return false;
    }

    while (*path == '/') {
        path++;
    }

    /* the root is not a directory entry anywhere, so it has to be made
     * up. everything else is found by walking */
    memset(out, 0, sizeof *out);
    out->first_cluster = fs->root_cluster;
    out->is_dir = true;
    out->attr = FAT32_ATTR_DIRECTORY;
    copy_name(out->name, "/");

    while (*path != '\0') {
        char component[FAT32_NAME_MAX];
        size_t n = 0;
        while (*path != '\0' && *path != '/' && n < FAT32_NAME_MAX - 1) {
            component[n++] = *path++;
        }
        component[n] = '\0';
        while (*path == '/') {
            path++;
        }
        if (n == 0) {
            continue;
        }

        if (!out->is_dir) {
            return false;       /* a file cannot have anything inside it */
        }

        struct fat32_file found;
        if (!find_in(fs, out->first_cluster, component, &found)) {
            return false;
        }
        *out = found;

        /* a subdirectory's dotdot points at the root as cluster 0, which
         * is a convention rather than a real cluster number */
        if (out->is_dir && out->first_cluster == 0) {
            out->first_cluster = fs->root_cluster;
        }
    }
    return true;
}

/* ---- reading ------------------------------------------------------- */

/* step along the chain to the cluster holding byte `offset` */
static bool seek_cluster(struct fat32 *fs, uint32_t start, uint64_t offset,
                         uint32_t *out) {
    uint32_t cluster = start;
    uint64_t skip = offset / fat32_cluster_bytes(fs);

    while (skip-- > 0) {
        uint32_t next;
        if (!fat_get(fs, cluster, &next) || next >= EOC) {
            return false;
        }
        cluster = next;
    }
    *out = cluster;
    return true;
}

int64_t fat32_read(struct fat32 *fs, const struct fat32_file *f,
                   uint64_t offset, void *buf, uint64_t len) {
    if (!fs->mounted || f->is_dir) {
        return -1;
    }
    if (offset >= f->size) {
        return 0;                       /* the end, which is not an error */
    }
    if (offset + len > f->size) {
        len = f->size - offset;         /* a short read, as usual */
    }

    uint32_t cluster;
    if (!cluster_ok(fs, f->first_cluster)
        || !seek_cluster(fs, f->first_cluster, offset, &cluster)) {
        return -1;
    }

    uint8_t *dst = buf;
    uint64_t done = 0;
    uint32_t within = (uint32_t)(offset % fat32_cluster_bytes(fs));

    while (done < len) {
        if (!cluster_ok(fs, cluster)) {
            return -1;
        }

        uint32_t sector = within / FAT32_SECTOR;
        uint32_t in_sector = within % FAT32_SECTOR;

        if (!read_sector(fs, cluster_lba(fs, cluster) + sector, fs->scratch)) {
            return -1;
        }

        uint64_t chunk = FAT32_SECTOR - in_sector;
        if (chunk > len - done) {
            chunk = len - done;
        }
        memcpy(dst + done, &fs->scratch[in_sector], chunk);
        done += chunk;
        within += (uint32_t)chunk;

        if (within >= fat32_cluster_bytes(fs) && done < len) {
            uint32_t next;
            if (!fat_get(fs, cluster, &next) || next >= EOC) {
                break;      /* the chain ended before the size said it would */
            }
            cluster = next;
            within = 0;
        }
    }
    return (int64_t)done;
}

/* ---- writing ------------------------------------------------------- */

/* go back to the record this file came from and correct it */
static bool update_entry(struct fat32 *fs, const struct fat32_file *f) {
    if (f->entry_sector == 0 || fs->write == NULL) {
        return false;
    }
    if (!read_sector(fs, f->entry_sector, fs->scratch)) {
        return false;
    }
    uint8_t *entry = &fs->scratch[f->entry_offset];
    wr32(&entry[28], f->size);
    wr16(&entry[20], (uint16_t)(f->first_cluster >> 16));
    wr16(&entry[26], (uint16_t)(f->first_cluster & 0xffff));
    return write_sector(fs, f->entry_sector, fs->scratch);
}

/* the cluster holding `offset`, adding one to the end of the chain if
 * the file does not reach that far yet */
static bool cluster_for_write(struct fat32 *fs, struct fat32_file *f,
                              uint64_t offset, uint32_t *out) {
    uint32_t per = fat32_cluster_bytes(fs);

    if (f->first_cluster == 0) {
        uint32_t fresh = alloc_cluster(fs);
        if (fresh == 0) {
            return false;
        }
        f->first_cluster = fresh;
    }

    uint32_t cluster = f->first_cluster;
    uint64_t steps = offset / per;

    while (steps-- > 0) {
        uint32_t next;
        if (!fat_get(fs, cluster, &next)) {
            return false;
        }
        if (next >= EOC) {
            uint32_t fresh = alloc_cluster(fs);
            if (fresh == 0 || !fat_set(fs, cluster, fresh)) {
                return false;
            }
            next = fresh;
        }
        cluster = next;
    }
    *out = cluster;
    return true;
}

int64_t fat32_write(struct fat32 *fs, struct fat32_file *f,
                    uint64_t offset, const void *buf, uint64_t len) {
    if (!fs->mounted || fs->write == NULL || f->is_dir) {
        return -1;
    }
    if (f->attr & FAT32_ATTR_READ_ONLY) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    const uint8_t *src = buf;
    uint64_t done = 0;

    while (done < len) {
        uint64_t at = offset + done;
        uint32_t cluster;
        if (!cluster_for_write(fs, f, at, &cluster)) {
            break;      /* the disk is full. keep what I managed */
        }

        uint32_t within = (uint32_t)(at % fat32_cluster_bytes(fs));
        uint64_t lba = cluster_lba(fs, cluster) + within / FAT32_SECTOR;
        uint32_t in_sector = within % FAT32_SECTOR;

        /* a partial sector has to be read before it is written, or the
         * bytes either side of mine would be replaced with nothing */
        uint64_t chunk = FAT32_SECTOR - in_sector;
        if (chunk > len - done) {
            chunk = len - done;
        }
        if (chunk != FAT32_SECTOR) {
            if (!read_sector(fs, lba, fs->scratch)) {
                break;
            }
        }
        memcpy(&fs->scratch[in_sector], src + done, chunk);
        if (!write_sector(fs, lba, fs->scratch)) {
            break;
        }
        done += chunk;
    }

    if (offset + done > f->size) {
        f->size = (uint32_t)(offset + done);
    }
    if (!update_entry(fs, f)) {
        return -1;
    }
    return (int64_t)done;
}

/* ---- making a file ------------------------------------------------- */

/* "notes.txt" -> "NOTES   TXT", plus the two bits recording that it was
 * lowercase. names that do not fit are refused rather than mangled */
static bool to_short(const char *name, uint8_t *out11, uint8_t *case_bits) {
    const char *dot = NULL;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len  = dot ? strlen(dot + 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) {
        return false;
    }

    bool base_lower = false, base_upper = false;
    bool ext_lower = false, ext_upper = false;

    for (int i = 0; i < 11; i++) {
        out11[i] = ' ';
    }
    for (size_t i = 0; i < base_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') { base_lower = true; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') { base_upper = true; }
        else if (!((c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~')) {
            return false;
        }
        out11[i] = (uint8_t)c;
    }
    for (size_t i = 0; i < ext_len; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') { ext_lower = true; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') { ext_upper = true; }
        else if (!((c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~')) {
            return false;
        }
        out11[8 + i] = (uint8_t)c;
    }

    /* mixed case in one part cannot be recorded by a single bit, so
     * such a name genuinely does need a long entry, and I refuse it */
    if ((base_lower && base_upper) || (ext_lower && ext_upper)) {
        return false;
    }

    *case_bits = (uint8_t)((base_lower ? 0x08 : 0) | (ext_lower ? 0x10 : 0));
    return true;
}

/* the first slot in a directory nobody is using, growing the directory
 * by a cluster if every slot is taken */
static bool free_slot(struct fat32 *fs, uint32_t dir_cluster,
                      uint64_t *lba_out, uint32_t *off_out) {
    uint32_t cluster = dir_cluster;

    for (;;) {
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            uint64_t lba = cluster_lba(fs, cluster) + s;
            if (!read_sector(fs, lba, fs->scratch)) {
                return false;
            }
            for (uint32_t off = 0; off < FAT32_SECTOR; off += 32) {
                if (fs->scratch[off] == 0x00 || fs->scratch[off] == 0xe5) {
                    *lba_out = lba;
                    *off_out = off;
                    return true;
                }
            }
        }

        uint32_t next;
        if (!fat_get(fs, cluster, &next)) {
            return false;
        }
        if (next >= EOC) {
            /* the directory is full. give it another cluster, zeroed,
             * since a directory's end is marked by a zero byte and
             * whatever was in that cluster before certainly is not */
            uint32_t fresh = alloc_cluster(fs);
            if (fresh == 0 || !fat_set(fs, cluster, fresh)) {
                return false;
            }
            memset(fs->scratch, 0, FAT32_SECTOR);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
                if (!write_sector(fs, cluster_lba(fs, fresh) + s, fs->scratch)) {
                    return false;
                }
            }
            next = fresh;
        }
        cluster = next;
    }
}

bool fat32_create(struct fat32 *fs, const char *path, struct fat32_file *out) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    /* split off the last component; the rest has to exist already */
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        return false;
    }

    char parent[FAT32_NAME_MAX];
    size_t plen = (size_t)(name - path);
    if (plen >= sizeof parent) {
        return false;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';

    struct fat32_file dir;
    if (!fat32_lookup(fs, parent, &dir) || !dir.is_dir) {
        return false;
    }

    /* already there? then this is just an open */
    if (find_in(fs, dir.first_cluster, name, out)) {
        return !out->is_dir;
    }

    uint8_t short11[11];
    uint8_t case_bits;
    if (!to_short(name, short11, &case_bits)) {
        return false;       /* a name I cannot store honestly */
    }

    uint64_t lba;
    uint32_t off;
    if (!free_slot(fs, dir.first_cluster, &lba, &off)) {
        return false;
    }

    /* whether this slot was the end of the directory matters: if it was,
     * the next slot has to be left as a zero to say so, and read_sector
     * has already given me a sector where it is */
    if (!read_sector(fs, lba, fs->scratch)) {
        return false;
    }
    uint8_t *entry = &fs->scratch[off];
    memset(entry, 0, 32);
    memcpy(entry, short11, 11);
    entry[11] = FAT32_ATTR_ARCHIVE;
    entry[12] = case_bits;
    if (!write_sector(fs, lba, fs->scratch)) {
        return false;
    }

    memset(out, 0, sizeof *out);
    short_name(entry, out->name);
    out->attr = FAT32_ATTR_ARCHIVE;
    out->is_dir = false;
    out->size = 0;
    out->first_cluster = 0;         /* an empty file owns no clusters */
    out->entry_sector = lba;
    out->entry_offset = off;
    return true;
}

/* ---- directories ---------------------------------------------------- */

/* a fresh cluster with nothing in it, which for a directory means every
 * byte zero -- the first zero byte is what says "no more entries" */
static bool blank_cluster(struct fat32 *fs, uint32_t cluster) {
    memset(fs->scratch, 0, FAT32_SECTOR);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        if (!write_sector(fs, cluster_lba(fs, cluster) + s, fs->scratch)) {
            return false;
        }
    }
    return true;
}

bool fat32_mkdir(struct fat32 *fs, const char *path) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    /* where it goes, and what it is called */
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        return false;
    }

    char parent[FAT32_NAME_MAX];
    size_t plen = (size_t)(name - path);
    if (plen >= sizeof parent) {
        return false;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';

    struct fat32_file dir;
    if (!fat32_lookup(fs, parent, &dir) || !dir.is_dir) {
        return false;
    }

    struct fat32_file existing;
    if (find_in(fs, dir.first_cluster, name, &existing)) {
        return false;       /* something is already called that */
    }

    uint8_t short11[11];
    uint8_t case_bits;
    if (!to_short(name, short11, &case_bits)) {
        return false;
    }

    uint32_t cluster = alloc_cluster(fs);
    if (cluster == 0 || !blank_cluster(fs, cluster)) {
        return false;
    }

    /* every directory but the root begins with two entries: itself, and
     * whatever it hangs from. the root is written as cluster 0 in the
     * second one -- a convention rather than a real cluster number,
     * which is why looking one up has to translate it back */
    memset(fs->scratch, 0, FAT32_SECTOR);
    uint8_t *dot = fs->scratch;
    memcpy(dot, ".          ", 11);
    dot[11] = FAT32_ATTR_DIRECTORY;
    wr16(&dot[20], (uint16_t)(cluster >> 16));
    wr16(&dot[26], (uint16_t)(cluster & 0xffff));

    uint8_t *dotdot = fs->scratch + 32;
    memcpy(dotdot, "..         ", 11);
    dotdot[11] = FAT32_ATTR_DIRECTORY;
    uint32_t up = (dir.first_cluster == fs->root_cluster) ? 0 : dir.first_cluster;
    wr16(&dotdot[20], (uint16_t)(up >> 16));
    wr16(&dotdot[26], (uint16_t)(up & 0xffff));

    if (!write_sector(fs, cluster_lba(fs, cluster), fs->scratch)) {
        return false;
    }

    /* and the entry in the parent that makes it findable */
    uint64_t lba;
    uint32_t off;
    if (!free_slot(fs, dir.first_cluster, &lba, &off)) {
        return false;
    }
    if (!read_sector(fs, lba, fs->scratch)) {
        return false;
    }
    uint8_t *entry = &fs->scratch[off];
    memset(entry, 0, 32);
    memcpy(entry, short11, 11);
    entry[11] = FAT32_ATTR_DIRECTORY;
    entry[12] = case_bits;
    wr16(&entry[20], (uint16_t)(cluster >> 16));
    wr16(&entry[26], (uint16_t)(cluster & 0xffff));
    return write_sector(fs, lba, fs->scratch);
}

bool fat32_rmdir(struct fat32 *fs, const char *path) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    struct fat32_file d;
    if (!fat32_lookup(fs, path, &d) || !d.is_dir) {
        return false;
    }
    if (d.first_cluster == fs->root_cluster || d.entry_sector == 0) {
        return false;       /* the root is nobody's to remove */
    }

    /* it has to be empty. dot and dotdot are skipped by readdir, which
     * is exactly the question being asked here */
    struct fat32_file ignored;
    if (fat32_readdir(fs, d.first_cluster, 0, &ignored)) {
        return false;
    }

    /* let the clusters go, following the chain rather than assuming one */
    uint32_t cluster = d.first_cluster;
    while (cluster_ok(fs, cluster)) {
        uint32_t next;
        if (!fat_get(fs, cluster, &next)) {
            break;
        }
        if (!fat_set(fs, cluster, 0)) {
            return false;
        }
        if (next >= EOC) {
            break;
        }
        cluster = next;
    }

    /* and strike the entry out. 0xe5 is how fat has always said "this
     * one is gone" without moving everything after it up */
    if (!read_sector(fs, d.entry_sector, fs->scratch)) {
        return false;
    }
    fs->scratch[d.entry_offset] = 0xe5;
    return write_sector(fs, d.entry_sector, fs->scratch);
}

/* ---- how full it is ------------------------------------------------ */

bool fat32_usage(struct fat32 *fs, uint32_t *used, uint32_t *total) {
    if (!fs->mounted) {
        return false;
    }
    *total = fs->cluster_count;
    *used = 0;

    /* walk the table rather than trusting fsinfo, which is a cache and
     * is wrong on any disk that was not put away tidily */
    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        uint32_t value;
        if (!fat_get(fs, c, &value)) {
            return false;
        }
        if (value != 0) {
            (*used)++;
        }
    }
    return true;
}
