#include "fs/ext2.h"
#include "lib/string.h"

/* ---- the on-disk shapes, read by hand ------------------------------
 *
 * by hand rather than as structs, for the same reason fat32 does it:
 * the layout is little-endian and packed, and a struct that happens to
 * match is a struct that stops matching the moment anybody changes a
 * compiler flag. offsets are the format; nothing else is. */

#define SB_OFFSET       1024
#define SB_MAGIC        0xef53

#define GD_SIZE         32
#define DIRENT_MIN      8

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint32_t ext2_block_bytes(const struct ext2 *fs) { return fs->block_size; }

/* ---- time ----------------------------------------------------------
 *
 * ext2 keeps seconds since 1970 and everything above wants a date. the
 * arithmetic is howard hinnant's days-from-civil, which is the version
 * that gets the leap year rule right without a table -- and a leap year
 * rule is exactly the kind of thing that is correct for four years */

void ext2_unix_to_time(uint32_t seconds, struct fat32_time *out) {
    uint32_t days = seconds / 86400;
    uint32_t rest = seconds % 86400;

    out->hour = (uint8_t)(rest / 3600);
    out->minute = (uint8_t)((rest % 3600) / 60);
    out->second = (uint8_t)(rest % 60);

    /* shift the epoch to 1st march 0000, which puts the leap day at the
     * end of the year and makes the month lengths a repeating pattern */
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m = mp + (mp < 10 ? 3 : -9);

    out->year = (uint16_t)(y + (m <= 2 ? 1 : 0));
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
}

uint32_t ext2_time_to_unix(const struct fat32_time *t) {
    int64_t y = t->year;
    unsigned m = t->month, d = t->day;
    if (m <= 2) {
        y--;
    }
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;

    return (uint32_t)(days * 86400 + t->hour * 3600 + t->minute * 60
                      + t->second);
}

/* ---- blocks --------------------------------------------------------- */

static bool read_block(struct ext2 *fs, uint32_t block, void *into) {
    if (block == 0 || block >= fs->blocks_count) {
        return false;
    }
    uint32_t per = fs->block_size / EXT2_SECTOR;
    return fs->read(fs->ctx, (uint64_t)block * per, per, into);
}

static bool write_block(struct ext2 *fs, uint32_t block, const void *from) {
    if (fs->write == NULL || block == 0 || block >= fs->blocks_count) {
        return false;
    }
    uint32_t per = fs->block_size / EXT2_SECTOR;
    return fs->write(fs->ctx, (uint64_t)block * per, per, from);
}

/* ---- group descriptors ---------------------------------------------- */

/* the descriptors live in a table of their own, and there is one per
 * group. reading a descriptor means reading the block it is in, which
 * is why this hands back the whole block and an offset rather than a
 * copy: the caller usually wants to change it and write it back */
static bool read_gd(struct ext2 *fs, uint32_t group, uint8_t *block,
                    uint32_t *offset) {
    uint32_t per_block = fs->block_size / GD_SIZE;
    uint32_t which = fs->gdt_block + group / per_block;
    if (!read_block(fs, which, block)) {
        return false;
    }
    *offset = (group % per_block) * GD_SIZE;
    return true;
}

static bool write_gd(struct ext2 *fs, uint32_t group, const uint8_t *block) {
    uint32_t per_block = fs->block_size / GD_SIZE;
    return write_block(fs, fs->gdt_block + group / per_block, block);
}

/* ---- the superblock, and the counts in it --------------------------- */

static bool flush_super(struct ext2 *fs) {
    if (fs->write == NULL) {
        return false;
    }
    /* the superblock is not block-aligned when blocks are 1 KiB -- it
     * is at byte 1024, which is block 1. read, change, write */
    uint32_t block = SB_OFFSET / fs->block_size;
    if (!read_block(fs, block, fs->scratch)) {
        return false;
    }
    uint8_t *sb = fs->scratch + (SB_OFFSET % fs->block_size);
    wr32(sb + 12, fs->free_blocks);
    wr32(sb + 16, fs->free_inodes);
    return write_block(fs, block, fs->scratch);
}

bool ext2_mount(struct ext2 *fs, ext2_io read, ext2_out write, void *ctx) {
    memset(fs, 0, sizeof *fs);
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;

    uint8_t sector[EXT2_SECTOR * 2];
    if (!read(ctx, SB_OFFSET / EXT2_SECTOR, 2, sector)) {
        return false;
    }
    const uint8_t *sb = sector;

    if (rd16(sb + 56) != SB_MAGIC) {
        return false;
    }

    uint32_t log_block = rd32(sb + 24);
    if (log_block > 2) {
        return false;       /* blocks bigger than I keep scratch for */
    }
    fs->block_size = 1024u << log_block;

    fs->inodes_count = rd32(sb + 0);
    fs->blocks_count = rd32(sb + 4);
    fs->free_blocks = rd32(sb + 12);
    fs->free_inodes = rd32(sb + 16);
    fs->first_data_block = rd32(sb + 20);
    fs->blocks_per_group = rd32(sb + 32);
    fs->inodes_per_group = rd32(sb + 40);

    uint32_t rev = rd32(sb + 76);
    if (rev >= 1) {
        fs->first_ino = rd32(sb + 84);
        fs->inode_size = rd16(sb + 88);

        /* a feature I do not implement is a filesystem I would be
         * guessing at, and guessing at a filesystem is how you write
         * over somebody's data. so: refuse, rather than try */
        uint32_t incompat = rd32(sb + 96);
        if (incompat & ~(uint32_t)EXT2_INCOMPAT_KNOWN) {
            return false;
        }
    } else {
        fs->first_ino = 11;
        fs->inode_size = 128;
    }

    if (fs->inode_size == 0 || fs->inode_size > fs->block_size
        || fs->blocks_per_group == 0 || fs->inodes_per_group == 0) {
        return false;
    }

    for (int i = 0; i < 16; i++) {
        fs->label[i] = (char)sb[120 + i];
    }
    fs->label[16] = '\0';

    fs->groups = (fs->blocks_count - fs->first_data_block
                  + fs->blocks_per_group - 1) / fs->blocks_per_group;
    fs->gdt_block = fs->first_data_block + 1;

    fs->mounted = true;
    return true;
}

void ext2_set_clock(struct ext2 *fs, ext2_clock clock) { fs->clock = clock; }

/* ---- inodes --------------------------------------------------------- */

/* an inode's 128 bytes, copied out. a copy rather than a pointer into
 * scratch, because almost everything that reads one then goes and reads
 * a block, which would overwrite it */
static bool read_inode(struct ext2 *fs, uint32_t ino, uint8_t *out) {
    if (ino == 0 || ino > fs->inodes_count) {
        return false;
    }
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    uint32_t off;
    if (!read_gd(fs, group, fs->scratch, &off)) {
        return false;
    }
    uint32_t table = rd32(fs->scratch + off + 8);

    uint32_t per_block = fs->block_size / fs->inode_size;
    uint32_t block = table + index / per_block;
    uint32_t within = (index % per_block) * fs->inode_size;

    if (!read_block(fs, block, fs->scratch)) {
        return false;
    }
    memcpy(out, fs->scratch + within, fs->inode_size);
    return true;
}

static bool write_inode(struct ext2 *fs, uint32_t ino, const uint8_t *raw) {
    if (fs->write == NULL || ino == 0 || ino > fs->inodes_count) {
        return false;
    }
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    uint32_t off;
    if (!read_gd(fs, group, fs->scratch, &off)) {
        return false;
    }
    uint32_t table = rd32(fs->scratch + off + 8);

    uint32_t per_block = fs->block_size / fs->inode_size;
    uint32_t block = table + index / per_block;
    uint32_t within = (index % per_block) * fs->inode_size;

    if (!read_block(fs, block, fs->scratch)) {
        return false;
    }
    memcpy(fs->scratch + within, raw, fs->inode_size);
    return write_block(fs, block, fs->scratch);
}

static bool is_fast_symlink(const uint8_t *inode) {
    /* the one place in ext2 where those fifteen words are not block
     * numbers at all. anything that walks them without asking first
     * reads the target as a list of addresses, and gets a filesystem
     * that looks corrupt */
    return (rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFLNK && rd32(inode + 4) < 60;
}

static void fill_file(struct ext2 *fs, uint32_t ino, const uint8_t *raw,
                      struct ext2_file *out) {
    (void)fs;
    memset(out, 0, sizeof *out);
    out->ino = ino;
    out->mode = rd16(raw);
    out->uid = rd16(raw + 2);
    out->size = rd32(raw + 4);
    out->gid = rd16(raw + 24);
    out->links = rd16(raw + 26);
    out->is_dir = (out->mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    out->is_symlink = (out->mode & EXT2_S_IFMT) == EXT2_S_IFLNK;

    ext2_unix_to_time(rd32(raw + 8), &out->accessed);
    ext2_unix_to_time(rd32(raw + 12), &out->created);
    ext2_unix_to_time(rd32(raw + 16), &out->modified);
}

static void stamp(struct ext2 *fs, uint8_t *raw, bool created) {
    if (fs->clock == NULL) {
        return;
    }
    uint32_t now = fs->clock();
    wr32(raw + 8, now);         /* accessed */
    wr32(raw + 16, now);        /* modified */
    if (created) {
        wr32(raw + 12, now);
    }
}

/* ---- bitmaps -------------------------------------------------------- */

static bool bitmap_take(struct ext2 *fs, uint32_t bitmap_block, uint32_t count,
                        uint32_t *found) {
    if (!read_block(fs, bitmap_block, fs->indirect)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (fs->indirect[i / 8] & (1u << (i % 8))) {
            continue;
        }
        fs->indirect[i / 8] |= (uint8_t)(1u << (i % 8));
        if (!write_block(fs, bitmap_block, fs->indirect)) {
            return false;
        }
        *found = i;
        return true;
    }
    return false;
}

static bool bitmap_give(struct ext2 *fs, uint32_t bitmap_block, uint32_t bit) {
    if (!read_block(fs, bitmap_block, fs->indirect)) {
        return false;
    }
    fs->indirect[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    return write_block(fs, bitmap_block, fs->indirect);
}

/* a free block, from whichever group has one. zeroed before it is
 * handed out, because a block still holding somebody else's data is
 * the oldest information leak there is */
static uint32_t alloc_block(struct ext2 *fs) {
    if (fs->write == NULL) {
        return 0;
    }
    for (uint32_t g = 0; g < fs->groups; g++) {
        uint32_t off;
        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        if (rd16(fs->scratch + off + 12) == 0) {
            continue;
        }
        uint32_t bitmap = rd32(fs->scratch + off + 0);

        uint32_t bit;
        if (!bitmap_take(fs, bitmap, fs->blocks_per_group, &bit)) {
            continue;
        }

        /* the descriptor was read into scratch and bitmap_take used
         * indirect, so scratch is still the descriptor block */
        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        wr16(fs->scratch + off + 12, (uint16_t)(rd16(fs->scratch + off + 12) - 1));
        if (!write_gd(fs, g, fs->scratch)) {
            return 0;
        }

        fs->free_blocks--;
        flush_super(fs);

        uint32_t block = fs->first_data_block + g * fs->blocks_per_group + bit;
        memset(fs->indirect, 0, fs->block_size);
        if (!write_block(fs, block, fs->indirect)) {
            return 0;
        }
        return block;
    }
    return 0;
}

static bool free_block(struct ext2 *fs, uint32_t block) {
    if (fs->write == NULL || block < fs->first_data_block
        || block >= fs->blocks_count) {
        return false;
    }
    uint32_t g = (block - fs->first_data_block) / fs->blocks_per_group;
    uint32_t bit = (block - fs->first_data_block) % fs->blocks_per_group;

    uint32_t off;
    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    uint32_t bitmap = rd32(fs->scratch + off + 0);
    if (!bitmap_give(fs, bitmap, bit)) {
        return false;
    }

    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    wr16(fs->scratch + off + 12, (uint16_t)(rd16(fs->scratch + off + 12) + 1));
    if (!write_gd(fs, g, fs->scratch)) {
        return false;
    }

    fs->free_blocks++;
    return flush_super(fs);
}

static uint32_t alloc_inode(struct ext2 *fs, bool directory) {
    if (fs->write == NULL) {
        return 0;
    }
    for (uint32_t g = 0; g < fs->groups; g++) {
        uint32_t off;
        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        if (rd16(fs->scratch + off + 14) == 0) {
            continue;
        }
        uint32_t bitmap = rd32(fs->scratch + off + 4);

        uint32_t bit;
        if (!bitmap_take(fs, bitmap, fs->inodes_per_group, &bit)) {
            continue;
        }

        uint32_t ino = g * fs->inodes_per_group + bit + 1;
        if (ino < fs->first_ino && ino != EXT2_ROOT_INO) {
            /* a reserved number. give it back and keep looking, rather
             * than handing out an inode the format has plans for */
            bitmap_give(fs, bitmap, bit);
            continue;
        }

        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        wr16(fs->scratch + off + 14,
             (uint16_t)(rd16(fs->scratch + off + 14) - 1));
        if (directory) {
            wr16(fs->scratch + off + 16,
                 (uint16_t)(rd16(fs->scratch + off + 16) + 1));
        }
        if (!write_gd(fs, g, fs->scratch)) {
            return 0;
        }

        fs->free_inodes--;
        flush_super(fs);
        return ino;
    }
    return 0;
}

static bool free_inode(struct ext2 *fs, uint32_t ino, bool directory) {
    uint32_t g = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;

    uint32_t off;
    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    uint32_t bitmap = rd32(fs->scratch + off + 4);
    if (!bitmap_give(fs, bitmap, bit)) {
        return false;
    }

    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    wr16(fs->scratch + off + 14, (uint16_t)(rd16(fs->scratch + off + 14) + 1));
    if (directory && rd16(fs->scratch + off + 16) > 0) {
        wr16(fs->scratch + off + 16,
             (uint16_t)(rd16(fs->scratch + off + 16) - 1));
    }
    if (!write_gd(fs, g, fs->scratch)) {
        return false;
    }

    fs->free_inodes++;
    return flush_super(fs);
}

/* ---- the block map --------------------------------------------------
 *
 * twelve direct, then one indirect, then double, then triple. the
 * arithmetic is the same at each level and the temptation is to write
 * it once and recurse -- but a recursive walk needs a block of scratch
 * per level, and there are two. so it is spelled out */

static uint32_t per_block(const struct ext2 *fs) { return fs->block_size / 4; }

/* the block holding the `index`th block of a file. `grow` says whether
 * to make one that is not there yet, which is the difference between
 * reading and writing */
static uint32_t map_block(struct ext2 *fs, uint8_t *inode, uint32_t index,
                          bool grow, bool *changed) {
    uint32_t n = per_block(fs);

    if (index < EXT2_DIRECT) {
        uint32_t b = rd32(inode + 40 + index * 4);
        if (b == 0 && grow) {
            b = alloc_block(fs);
            if (b == 0) {
                return 0;
            }
            wr32(inode + 40 + index * 4, b);
            *changed = true;
        }
        return b;
    }
    index -= EXT2_DIRECT;

    if (index < n) {
        uint32_t ind = rd32(inode + 40 + 12 * 4);
        if (ind == 0) {
            if (!grow) {
                return 0;
            }
            ind = alloc_block(fs);
            if (ind == 0) {
                return 0;
            }
            wr32(inode + 40 + 12 * 4, ind);
            *changed = true;
        }
        if (!read_block(fs, ind, fs->indirect)) {
            return 0;
        }
        uint32_t b = rd32(fs->indirect + index * 4);
        if (b == 0 && grow) {
            b = alloc_block(fs);
            if (b == 0) {
                return 0;
            }
            /* alloc_block used `indirect` to zero the new block, so it
             * has to be read again before being changed */
            if (!read_block(fs, ind, fs->indirect)) {
                return 0;
            }
            wr32(fs->indirect + index * 4, b);
            if (!write_block(fs, ind, fs->indirect)) {
                return 0;
            }
        }
        return b;
    }
    index -= n;

    if (index < n * n) {
        uint32_t dind = rd32(inode + 40 + 13 * 4);
        if (dind == 0) {
            if (!grow) {
                return 0;
            }
            dind = alloc_block(fs);
            if (dind == 0) {
                return 0;
            }
            wr32(inode + 40 + 13 * 4, dind);
            *changed = true;
        }
        if (!read_block(fs, dind, fs->indirect)) {
            return 0;
        }
        uint32_t which = index / n;
        uint32_t ind = rd32(fs->indirect + which * 4);
        if (ind == 0) {
            if (!grow) {
                return 0;
            }
            ind = alloc_block(fs);
            if (ind == 0) {
                return 0;
            }
            if (!read_block(fs, dind, fs->indirect)) {
                return 0;
            }
            wr32(fs->indirect + which * 4, ind);
            if (!write_block(fs, dind, fs->indirect)) {
                return 0;
            }
        }
        if (!read_block(fs, ind, fs->indirect)) {
            return 0;
        }
        uint32_t at = index % n;
        uint32_t b = rd32(fs->indirect + at * 4);
        if (b == 0 && grow) {
            b = alloc_block(fs);
            if (b == 0) {
                return 0;
            }
            if (!read_block(fs, ind, fs->indirect)) {
                return 0;
            }
            wr32(fs->indirect + at * 4, b);
            if (!write_block(fs, ind, fs->indirect)) {
                return 0;
            }
        }
        return b;
    }

    /* triple indirection reaches sixteen gigabytes with these blocks,
     * and nothing here is going to. saying so beats pretending */
    return 0;
}

/* every block of a file, given back one at a time. used by free_blocks
 * and by nothing else, so it walks rather than maps */
static void free_all_blocks(struct ext2 *fs, uint8_t *inode) {
    if (is_fast_symlink(inode)) {
        return;
    }
    uint32_t n = per_block(fs);

    for (uint32_t i = 0; i < EXT2_DIRECT; i++) {
        uint32_t b = rd32(inode + 40 + i * 4);
        if (b) {
            free_block(fs, b);
            wr32(inode + 40 + i * 4, 0);
        }
    }

    uint32_t ind = rd32(inode + 40 + 12 * 4);
    if (ind) {
        if (read_block(fs, ind, fs->indirect)) {
            /* copied out, because freeing each one reads the bitmap
             * into the same buffer */
            static uint32_t list[EXT2_MAX_BLOCK / 4];
            for (uint32_t i = 0; i < n; i++) {
                list[i] = rd32(fs->indirect + i * 4);
            }
            for (uint32_t i = 0; i < n; i++) {
                if (list[i]) {
                    free_block(fs, list[i]);
                }
            }
        }
        free_block(fs, ind);
        wr32(inode + 40 + 12 * 4, 0);
    }

    uint32_t dind = rd32(inode + 40 + 13 * 4);
    if (dind) {
        static uint32_t outer[EXT2_MAX_BLOCK / 4];
        if (read_block(fs, dind, fs->indirect)) {
            for (uint32_t i = 0; i < n; i++) {
                outer[i] = rd32(fs->indirect + i * 4);
            }
            for (uint32_t i = 0; i < n; i++) {
                if (!outer[i]) {
                    continue;
                }
                if (read_block(fs, outer[i], fs->indirect)) {
                    static uint32_t inner[EXT2_MAX_BLOCK / 4];
                    for (uint32_t j = 0; j < n; j++) {
                        inner[j] = rd32(fs->indirect + j * 4);
                    }
                    for (uint32_t j = 0; j < n; j++) {
                        if (inner[j]) {
                            free_block(fs, inner[j]);
                        }
                    }
                }
                free_block(fs, outer[i]);
            }
        }
        free_block(fs, dind);
        wr32(inode + 40 + 13 * 4, 0);
    }
}

/* ---- reading and writing bytes -------------------------------------- */

static int64_t read_bytes(struct ext2 *fs, uint8_t *inode, uint64_t offset,
                          void *buf, uint64_t len) {
    uint64_t size = rd32(inode + 4);

    if (is_fast_symlink(inode)) {
        if (offset >= size) {
            return 0;
        }
        if (offset + len > size) {
            len = size - offset;
        }
        memcpy(buf, inode + 40 + offset, len);
        return (int64_t)len;
    }

    if (offset >= size) {
        return 0;
    }
    if (offset + len > size) {
        len = size - offset;
    }

    uint8_t *out = buf;
    uint64_t done = 0;
    bool changed = false;

    while (done < len) {
        uint64_t at = offset + done;
        uint32_t index = (uint32_t)(at / fs->block_size);
        uint32_t within = (uint32_t)(at % fs->block_size);

        uint32_t take = fs->block_size - within;
        if (take > len - done) {
            take = (uint32_t)(len - done);
        }

        uint32_t block = map_block(fs, inode, index, false, &changed);
        if (block == 0) {
            /* a hole. ext2 allows them and they read as zeroes, which
             * is not the same as an error and must not be treated as
             * one */
            memset(out + done, 0, take);
        } else {
            if (!read_block(fs, block, fs->scratch)) {
                return done > 0 ? (int64_t)done : -1;
            }
            memcpy(out + done, fs->scratch + within, take);
        }
        done += take;
    }
    return (int64_t)done;
}

static int64_t write_bytes(struct ext2 *fs, uint32_t ino, uint8_t *inode,
                           uint64_t offset, const void *buf, uint64_t len) {
    if (fs->write == NULL) {
        return -1;
    }
    if ((rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        return -1;
    }

    const uint8_t *in = buf;
    uint64_t done = 0;
    bool changed = false;

    while (done < len) {
        uint64_t at = offset + done;
        uint32_t index = (uint32_t)(at / fs->block_size);
        uint32_t within = (uint32_t)(at % fs->block_size);

        uint32_t take = fs->block_size - within;
        if (take > len - done) {
            take = (uint32_t)(len - done);
        }

        uint32_t block = map_block(fs, inode, index, true, &changed);
        if (block == 0) {
            break;      /* out of room. keep what went */
        }

        if (take == fs->block_size) {
            memcpy(fs->scratch, in + done, take);
        } else {
            if (!read_block(fs, block, fs->scratch)) {
                break;
            }
            memcpy(fs->scratch + within, in + done, take);
        }
        if (!write_block(fs, block, fs->scratch)) {
            break;
        }
        done += take;
    }

    uint64_t size = rd32(inode + 4);
    if (offset + done > size) {
        wr32(inode + 4, (uint32_t)(offset + done));
    }
    stamp(fs, inode, false);
    if (!write_inode(fs, ino, inode)) {
        return -1;
    }
    return (int64_t)done;
}

/* ---- directories ----------------------------------------------------
 *
 * a directory is a file whose contents are entries, and every entry's
 * rec_len says where the next one starts. the last entry in each block
 * stretches to the end of it -- which is what makes deleting one a
 * matter of widening the entry before it rather than moving anything */

struct dirent_at {
    uint32_t block_index;       /* which block of the directory */
    uint32_t offset;            /* where in it */
    uint32_t prev_offset;       /* the entry before, or the same if first */
    uint32_t ino;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
};

static uint32_t entry_size(uint8_t name_len) {
    return ((uint32_t)DIRENT_MIN + name_len + 3) & ~3u;
}

/* walk every entry of a directory, calling nothing -- the caller drives
 * it by index so that the block stays in scratch between calls */
static bool dir_scan(struct ext2 *fs, uint8_t *dir_inode, const char *want,
                     size_t want_index, struct dirent_at *out, char *name_out) {
    uint64_t size = rd32(dir_inode + 4);
    uint32_t blocks = (uint32_t)((size + fs->block_size - 1) / fs->block_size);
    bool changed = false;
    size_t seen = 0;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        uint32_t block = map_block(fs, dir_inode, bi, false, &changed);
        if (block == 0) {
            continue;
        }
        if (!read_block(fs, block, fs->scratch)) {
            return false;
        }

        uint32_t at = 0;
        uint32_t prev = 0;
        while (at + DIRENT_MIN <= fs->block_size) {
            uint32_t ino = rd32(fs->scratch + at);
            uint16_t rec = rd16(fs->scratch + at + 4);
            uint8_t nlen = fs->scratch[at + 6];

            if (rec < DIRENT_MIN || at + rec > fs->block_size) {
                break;      /* a directory I do not believe */
            }

            if (ino != 0) {
                bool dot = (nlen == 1 && fs->scratch[at + 8] == '.')
                        || (nlen == 2 && fs->scratch[at + 8] == '.'
                            && fs->scratch[at + 9] == '.');

                bool match;
                if (want != NULL) {
                    match = true;
                    for (uint8_t i = 0; i < nlen && match; i++) {
                        if (want[i] == '\0' || want[i] != (char)fs->scratch[at + 8 + i]) {
                            match = false;
                        }
                    }
                    if (match && want[nlen] != '\0') {
                        match = false;
                    }
                } else {
                    match = !dot && (seen++ == want_index);
                }

                if (match) {
                    out->block_index = bi;
                    out->offset = at;
                    out->prev_offset = prev;
                    out->ino = ino;
                    out->rec_len = rec;
                    out->name_len = nlen;
                    out->file_type = fs->scratch[at + 7];
                    if (name_out != NULL) {
                        memcpy(name_out, fs->scratch + at + 8, nlen);
                        name_out[nlen] = '\0';
                    }
                    return true;
                }
            }

            prev = at;
            at += rec;
        }
    }
    return false;
}

static bool dir_find(struct ext2 *fs, uint8_t *dir_inode, const char *name,
                     struct dirent_at *out) {
    return dir_scan(fs, dir_inode, name, 0, out, NULL);
}

/* put a name in a directory. finds an entry with enough slack and
 * splits it, or adds a block */
static bool dir_add(struct ext2 *fs, uint32_t dir_ino, uint8_t *dir_inode,
                    const char *name, uint32_t ino, uint8_t file_type) {
    uint32_t nlen = 0;
    while (name[nlen] != '\0') {
        nlen++;
    }
    if (nlen == 0 || nlen > EXT2_NAME_MAX) {
        return false;
    }
    uint32_t need = entry_size((uint8_t)nlen);

    uint64_t size = rd32(dir_inode + 4);
    uint32_t blocks = (uint32_t)((size + fs->block_size - 1) / fs->block_size);
    bool changed = false;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        uint32_t block = map_block(fs, dir_inode, bi, false, &changed);
        if (block == 0 || !read_block(fs, block, fs->scratch)) {
            continue;
        }

        uint32_t at = 0;
        while (at + DIRENT_MIN <= fs->block_size) {
            uint32_t e_ino = rd32(fs->scratch + at);
            uint16_t rec = rd16(fs->scratch + at + 4);
            uint8_t e_nlen = fs->scratch[at + 6];
            if (rec < DIRENT_MIN || at + rec > fs->block_size) {
                break;
            }

            uint32_t used = (e_ino == 0) ? 0 : entry_size(e_nlen);
            if (rec - used >= need) {
                uint32_t put = at + used;
                if (used > 0) {
                    wr16(fs->scratch + at + 4, (uint16_t)used);
                }
                wr32(fs->scratch + put, ino);
                wr16(fs->scratch + put + 4, (uint16_t)(rec - used));
                fs->scratch[put + 6] = (uint8_t)nlen;
                fs->scratch[put + 7] = file_type;
                memcpy(fs->scratch + put + 8, name, nlen);
                return write_block(fs, block, fs->scratch);
            }
            at += rec;
        }
    }

    /* nowhere it fits: another block, entirely this entry */
    uint32_t block = map_block(fs, dir_inode, blocks, true, &changed);
    if (block == 0) {
        return false;
    }
    memset(fs->scratch, 0, fs->block_size);
    wr32(fs->scratch + 0, ino);
    wr16(fs->scratch + 4, (uint16_t)fs->block_size);
    fs->scratch[6] = (uint8_t)nlen;
    fs->scratch[7] = file_type;
    memcpy(fs->scratch + 8, name, nlen);
    if (!write_block(fs, block, fs->scratch)) {
        return false;
    }

    wr32(dir_inode + 4, (uint32_t)((blocks + 1) * fs->block_size));
    return write_inode(fs, dir_ino, dir_inode);
}

static bool dir_remove(struct ext2 *fs, uint8_t *dir_inode,
                       const struct dirent_at *where) {
    bool changed = false;
    uint32_t block = map_block(fs, dir_inode, where->block_index, false,
                               &changed);
    if (block == 0 || !read_block(fs, block, fs->scratch)) {
        return false;
    }

    if (where->offset == where->prev_offset) {
        /* the first entry in its block. there is nothing before it to
         * widen, so it is emptied in place -- an inode of zero is how
         * ext2 spells "skip this" */
        wr32(fs->scratch + where->offset, 0);
    } else {
        uint16_t prev_rec = rd16(fs->scratch + where->prev_offset + 4);
        wr16(fs->scratch + where->prev_offset + 4,
             (uint16_t)(prev_rec + where->rec_len));
    }
    return write_block(fs, block, fs->scratch);
}

static bool dir_is_empty(struct ext2 *fs, uint32_t ino) {
    uint8_t inode[256];
    if (!read_inode(fs, ino, inode)) {
        return false;
    }
    struct dirent_at at;
    return !dir_scan(fs, inode, NULL, 0, &at, NULL);
}

/* ---- paths ----------------------------------------------------------- */

#define SYMLINK_MAX 8

static bool resolve(struct ext2 *fs, const char *path, bool follow_last,
                    uint32_t *out_ino, uint8_t *out_inode, char *out_name);

static bool read_link_target(struct ext2 *fs, uint8_t *inode, char *out,
                             size_t size) {
    uint64_t len = rd32(inode + 4);
    if (len >= size) {
        return false;
    }
    if (read_bytes(fs, inode, 0, out, len) != (int64_t)len) {
        return false;
    }
    out[len] = '\0';
    return true;
}

/* walk a path from the root. `follow_last` decides whether a symlink at
 * the end is followed -- `cat` wants the file, `rm` wants the link */
static bool resolve(struct ext2 *fs, const char *path, bool follow_last,
                    uint32_t *out_ino, uint8_t *out_inode, char *out_name) {
    uint32_t ino = EXT2_ROOT_INO;
    uint8_t inode[256];
    if (!read_inode(fs, ino, inode)) {
        return false;
    }
    if (out_name != NULL) {
        out_name[0] = '\0';
    }

    int followed = 0;
    const char *p = path;

    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        char part[EXT2_NAME_MAX + 1];
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '/') {
            if (n >= EXT2_NAME_MAX) {
                return false;
            }
            part[n] = p[n];
            n++;
        }
        part[n] = '\0';
        const char *rest = p + n;
        bool last = true;
        for (const char *q = rest; *q != '\0'; q++) {
            if (*q != '/') {
                last = false;
                break;
            }
        }

        if ((rd16(inode) & EXT2_S_IFMT) != EXT2_S_IFDIR) {
            return false;
        }

        struct dirent_at at;
        if (!dir_find(fs, inode, part, &at)) {
            return false;
        }
        ino = at.ino;
        if (!read_inode(fs, ino, inode)) {
            return false;
        }
        if (out_name != NULL) {
            memcpy(out_name, part, n + 1);
        }

        /* a symlink in the middle is always followed; one at the end
         * only if asked. either way there is a limit, because a link
         * pointing at itself is a filesystem asking to be walked
         * forever */
        if ((rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFLNK
            && (!last || follow_last)) {
            if (++followed > SYMLINK_MAX) {
                return false;
            }
            char target[EXT2_NAME_MAX + 1];
            if (!read_link_target(fs, inode, target, sizeof target)) {
                return false;
            }
            if (target[0] == '/') {
                /* an absolute target starts again from the root */
                ino = EXT2_ROOT_INO;
                if (!read_inode(fs, ino, inode)) {
                    return false;
                }
                if (!resolve(fs, target, true, &ino, inode, out_name)) {
                    return false;
                }
            } else {
                /* relative: resolve it against where the link lives,
                 * which is the directory this walk was standing in.
                 * rebuilding that path is more bookkeeping than this
                 * needs, so a relative target is resolved from the
                 * parent by walking it fresh */
                char here[EXT2_NAME_MAX * 2 + 2];
                size_t upto = (size_t)(p - path);
                if (upto + n + 4 >= sizeof here) {
                    return false;
                }
                memcpy(here, path, upto);
                here[upto] = '\0';
                size_t at_end = upto;
                const char *t = target;
                while (*t != '\0' && at_end + 1 < sizeof here) {
                    here[at_end++] = *t++;
                }
                here[at_end] = '\0';
                if (!resolve(fs, here, true, &ino, inode, out_name)) {
                    return false;
                }
            }
        }

        p = rest;
    }

    *out_ino = ino;
    memcpy(out_inode, inode, fs->inode_size);
    return true;
}

bool ext2_lookup(struct ext2 *fs, const char *path, struct ext2_file *out) {
    if (!fs->mounted) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT2_NAME_MAX + 1];
    if (!resolve(fs, path, true, &ino, inode, name)) {
        return false;
    }
    fill_file(fs, ino, inode, out);
    memcpy(out->name, name, sizeof name > sizeof out->name
                            ? sizeof out->name : sizeof name);
    return true;
}

bool ext2_lookup_nofollow(struct ext2 *fs, const char *path,
                          struct ext2_file *out) {
    if (!fs->mounted) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT2_NAME_MAX + 1];
    if (!resolve(fs, path, false, &ino, inode, name)) {
        return false;
    }
    fill_file(fs, ino, inode, out);
    memcpy(out->name, name, sizeof name > sizeof out->name
                            ? sizeof out->name : sizeof name);
    return true;
}

bool ext2_readlink(struct ext2 *fs, const struct ext2_file *f, char *out,
                   size_t size) {
    if (!f->is_symlink) {
        return false;
    }
    uint8_t inode[256];
    if (!read_inode(fs, f->ino, inode)) {
        return false;
    }
    return read_link_target(fs, inode, out, size);
}

bool ext2_readdir(struct ext2 *fs, uint32_t dir_ino, size_t index,
                  struct ext2_file *out) {
    if (!fs->mounted) {
        return false;
    }
    if (dir_ino == 0) {
        dir_ino = EXT2_ROOT_INO;
    }

    uint8_t dir_inode[256];
    if (!read_inode(fs, dir_ino, dir_inode)) {
        return false;
    }

    struct dirent_at at;
    char name[EXT2_NAME_MAX + 1];
    if (!dir_scan(fs, dir_inode, NULL, index, &at, name)) {
        return false;
    }

    uint8_t inode[256];
    if (!read_inode(fs, at.ino, inode)) {
        return false;
    }
    fill_file(fs, at.ino, inode, out);

    size_t n = 0;
    while (name[n] != '\0' && n < EXT2_NAME_MAX) {
        out->name[n] = name[n];
        n++;
    }
    out->name[n] = '\0';
    return true;
}

int64_t ext2_read(struct ext2 *fs, const struct ext2_file *f, uint64_t offset,
                  void *buf, uint64_t len) {
    if (!fs->mounted) {
        return -1;
    }
    uint8_t inode[256];
    if (!read_inode(fs, f->ino, inode)) {
        return -1;
    }
    return read_bytes(fs, inode, offset, buf, len);
}

int64_t ext2_write(struct ext2 *fs, struct ext2_file *f, uint64_t offset,
                   const void *buf, uint64_t len) {
    if (!fs->mounted) {
        return -1;
    }
    uint8_t inode[256];
    if (!read_inode(fs, f->ino, inode)) {
        return -1;
    }
    int64_t n = write_bytes(fs, f->ino, inode, offset, buf, len);
    if (n > 0) {
        f->size = rd32(inode + 4);
    }
    return n;
}

/* ---- making things --------------------------------------------------- */

/* split a path into the directory holding it and the last name */
static bool split_parent(struct ext2 *fs, const char *path, uint32_t *dir_ino,
                         uint8_t *dir_inode, char *leaf) {
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        return false;
    }

    size_t n = 0;
    while (name[n] != '\0') {
        if (n >= EXT2_NAME_MAX) {
            return false;
        }
        leaf[n] = name[n];
        n++;
    }
    leaf[n] = '\0';

    char parent[EXT2_NAME_MAX * 2 + 2];
    size_t plen = (size_t)(name - path);
    if (plen >= sizeof parent) {
        return false;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';

    char ignored[EXT2_NAME_MAX + 1];
    if (!resolve(fs, parent, true, dir_ino, dir_inode, ignored)) {
        return false;
    }
    return (rd16(dir_inode) & EXT2_S_IFMT) == EXT2_S_IFDIR;
}

static bool make(struct ext2 *fs, const char *path, uint32_t mode,
                 uint32_t uid, uint32_t gid, const char *link_target,
                 uint32_t *out_ino) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    uint32_t dir_ino;
    uint8_t dir_inode[256];
    char leaf[EXT2_NAME_MAX + 1];
    if (!split_parent(fs, path, &dir_ino, dir_inode, leaf)) {
        return false;
    }

    struct dirent_at at;
    if (dir_find(fs, dir_inode, leaf, &at)) {
        return false;       /* already there */
    }

    bool directory = (mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    uint32_t ino = alloc_inode(fs, directory);
    if (ino == 0) {
        return false;
    }

    uint8_t inode[256];
    memset(inode, 0, sizeof inode);
    wr16(inode + 0, (uint16_t)mode);
    wr16(inode + 2, (uint16_t)uid);
    wr16(inode + 24, (uint16_t)gid);
    wr16(inode + 26, directory ? 2 : 1);
    stamp(fs, inode, true);

    if (link_target != NULL) {
        size_t len = 0;
        while (link_target[len] != '\0') {
            len++;
        }
        wr32(inode + 4, (uint32_t)len);
        if (len < 60) {
            memcpy(inode + 40, link_target, len);
        } else {
            if (!write_inode(fs, ino, inode)) {
                return false;
            }
            if (write_bytes(fs, ino, inode, 0, link_target, len)
                != (int64_t)len) {
                return false;
            }
            wr32(inode + 4, (uint32_t)len);
        }
    }

    if (!write_inode(fs, ino, inode)) {
        return false;
    }

    uint8_t file_type = directory ? 2 : (link_target != NULL ? 7 : 1);
    if (!dir_add(fs, dir_ino, dir_inode, leaf, ino, file_type)) {
        return false;
    }

    if (directory) {
        /* a directory is born with the two entries every directory has */
        bool changed = false;
        uint32_t block = map_block(fs, inode, 0, true, &changed);
        if (block == 0) {
            return false;
        }
        memset(fs->scratch, 0, fs->block_size);
        uint32_t dot = entry_size(1);
        wr32(fs->scratch + 0, ino);
        wr16(fs->scratch + 4, (uint16_t)dot);
        fs->scratch[6] = 1;
        fs->scratch[7] = 2;
        fs->scratch[8] = '.';
        wr32(fs->scratch + dot, dir_ino);
        wr16(fs->scratch + dot + 4, (uint16_t)(fs->block_size - dot));
        fs->scratch[dot + 6] = 2;
        fs->scratch[dot + 7] = 2;
        fs->scratch[dot + 8] = '.';
        fs->scratch[dot + 9] = '.';
        if (!write_block(fs, block, fs->scratch)) {
            return false;
        }
        wr32(inode + 4, fs->block_size);
        if (!write_inode(fs, ino, inode)) {
            return false;
        }

        /* the parent gained a child, and a child's `..` is a link to it */
        wr16(dir_inode + 26, (uint16_t)(rd16(dir_inode + 26) + 1));
        if (!write_inode(fs, dir_ino, dir_inode)) {
            return false;
        }
    }

    if (out_ino != NULL) {
        *out_ino = ino;
    }
    return true;
}

bool ext2_create(struct ext2 *fs, const char *path, uint32_t mode,
                 uint32_t uid, uint32_t gid, struct ext2_file *out) {
    /* already there? then this is an open, which is what create has
     * always meant when the thing exists */
    if (ext2_lookup(fs, path, out) && !out->is_dir) {
        return true;
    }

    uint32_t ino;
    if (!make(fs, path, EXT2_S_IFREG | (mode & 0xfff), uid, gid, NULL, &ino)) {
        return false;
    }
    return ext2_lookup(fs, path, out);
}

bool ext2_mkdir(struct ext2 *fs, const char *path, uint32_t mode,
                uint32_t uid, uint32_t gid) {
    return make(fs, path, EXT2_S_IFDIR | (mode & 0xfff), uid, gid, NULL, NULL);
}

bool ext2_symlink(struct ext2 *fs, const char *path, const char *target,
                  uint32_t uid, uint32_t gid) {
    return make(fs, path, EXT2_S_IFLNK | 0777, uid, gid, target, NULL);
}

/* ---- unmaking them --------------------------------------------------- */

static bool drop_name(struct ext2 *fs, const char *path, bool want_dir) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    uint32_t dir_ino;
    uint8_t dir_inode[256];
    char leaf[EXT2_NAME_MAX + 1];
    if (!split_parent(fs, path, &dir_ino, dir_inode, leaf)) {
        return false;
    }

    struct dirent_at at;
    if (!dir_find(fs, dir_inode, leaf, &at)) {
        return false;
    }

    uint8_t inode[256];
    if (!read_inode(fs, at.ino, inode)) {
        return false;
    }
    bool is_dir = (rd16(inode) & EXT2_S_IFMT) == EXT2_S_IFDIR;
    if (is_dir != want_dir) {
        return false;
    }
    if (is_dir && !dir_is_empty(fs, at.ino)) {
        return false;
    }

    if (!dir_remove(fs, dir_inode, &at)) {
        return false;
    }

    /* the name is gone. the *file* only goes when the last name does --
     * which is the whole reason a link count exists, and a sentence fat
     * cannot say at all */
    uint16_t links = rd16(inode + 26);
    if (is_dir) {
        links = 0;      /* its own `.` and its parent's entry both went */
        wr16(dir_inode + 26, (uint16_t)(rd16(dir_inode + 26) - 1));
        write_inode(fs, dir_ino, dir_inode);
    } else if (links > 0) {
        links--;
    }
    wr16(inode + 26, links);

    if (links == 0) {
        free_all_blocks(fs, inode);
        if (fs->clock != NULL) {
            wr32(inode + 20, fs->clock());     /* deleted at */
        }
        wr32(inode + 4, 0);
        write_inode(fs, at.ino, inode);
        return free_inode(fs, at.ino, is_dir);
    }
    return write_inode(fs, at.ino, inode);
}

bool ext2_unlink(struct ext2 *fs, const char *path) {
    return drop_name(fs, path, false);
}

bool ext2_rmdir(struct ext2 *fs, const char *path) {
    return drop_name(fs, path, true);
}

bool ext2_rename(struct ext2 *fs, const char *from, const char *to) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    uint32_t from_dir;
    uint8_t from_inode[256];
    char from_leaf[EXT2_NAME_MAX + 1];
    if (!split_parent(fs, from, &from_dir, from_inode, from_leaf)) {
        return false;
    }

    struct dirent_at at;
    if (!dir_find(fs, from_inode, from_leaf, &at)) {
        return false;
    }
    uint32_t ino = at.ino;
    uint8_t file_type = at.file_type;

    uint32_t to_dir;
    uint8_t to_inode[256];
    char to_leaf[EXT2_NAME_MAX + 1];
    if (!split_parent(fs, to, &to_dir, to_inode, to_leaf)) {
        return false;
    }

    struct dirent_at exists;
    if (dir_find(fs, to_inode, to_leaf, &exists)) {
        return false;       /* the new name is taken */
    }

    /* the new name first, then the old one struck out. a machine that
     * dies between the two leaves a file with two names, which fsck can
     * make sense of -- the other order leaves it with none */
    if (!dir_add(fs, to_dir, to_inode, to_leaf, ino, file_type)) {
        return false;
    }

    /* the parent's inode may have changed under dir_add, so read it
     * again before removing from it */
    if (!read_inode(fs, from_dir, from_inode)) {
        return false;
    }
    if (!dir_find(fs, from_inode, from_leaf, &at)) {
        return false;
    }
    return dir_remove(fs, from_inode, &at);
}

/* ---- the things fat had nowhere to write down ------------------------ */

bool ext2_chmod(struct ext2 *fs, const char *path, uint32_t mode) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT2_NAME_MAX + 1];
    if (!resolve(fs, path, true, &ino, inode, name)) {
        return false;
    }
    wr16(inode + 0, (uint16_t)((rd16(inode) & EXT2_S_IFMT) | (mode & 0xfff)));
    if (fs->clock != NULL) {
        wr32(inode + 12, fs->clock());      /* changed, not modified */
    }
    return write_inode(fs, ino, inode);
}

bool ext2_chown(struct ext2 *fs, const char *path, uint32_t uid,
                uint32_t gid) {
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT2_NAME_MAX + 1];
    if (!resolve(fs, path, true, &ino, inode, name)) {
        return false;
    }
    wr16(inode + 2, (uint16_t)uid);
    wr16(inode + 24, (uint16_t)gid);
    if (fs->clock != NULL) {
        wr32(inode + 12, fs->clock());
    }
    return write_inode(fs, ino, inode);
}

bool ext2_usage(struct ext2 *fs, uint64_t *used_bytes, uint64_t *total_bytes) {
    if (!fs->mounted) {
        return false;
    }
    uint64_t total = (uint64_t)fs->blocks_count * fs->block_size;
    uint64_t free_now = (uint64_t)fs->free_blocks * fs->block_size;
    *total_bytes = total;
    *used_bytes = total - free_now;
    return true;
}
