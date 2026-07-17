#include "drivers/part.h"
#include "lib/string.h"

/* ---- crc32 -----------------------------------------------------------
 *
 * the reflected one everybody means when they say crc32. computed a
 * nibble at a time out of a sixteen-entry table, which is a tenth the
 * memory of the usual 256-entry one and no slower than it needs to be:
 * this runs twice per disk at boot, and never again. */

static const uint32_t crc_nibble[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

uint32_t part_crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
    }
    return ~crc;
}

/* ---- reading the little-endian numbers a table is made of ----------- */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

const char *part_mbr_kind(uint8_t type) {
    switch (type) {
    case 0x00: return "empty";
    case 0x01: case 0x04: case 0x06: case 0x0e: return "fat16";
    case 0x0b: case 0x0c: return "fat32";
    case 0x05: case 0x0f: return "extended";
    case 0x07: return "ntfs/exfat";
    case 0x82: return "linux swap";
    case 0x83: return "linux";
    case 0xee: return "gpt protective";
    case 0xef: return "efi system";
    default:   return "unknown";
    }
}

/* ---- gpt type guids --------------------------------------------------
 *
 * stored the way microsoft writes guids: the first three fields
 * little-endian and the last two big-endian, which is why these look
 * shuffled next to the printed form. comparing raw bytes sidesteps the
 * whole question */
struct guid_name {
    uint8_t bytes[16];
    const char *name;
};

static const struct guid_name gpt_kinds[] = {
    /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
    { { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
        0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 }, "linux" },
    /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
    { { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b }, "efi system" },
    /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
    { { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 }, "windows data" },
    /* 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F */
    { { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
        0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f }, "linux swap" },
};

static const char *gpt_kind(const uint8_t *guid) {
    for (size_t i = 0; i < sizeof gpt_kinds / sizeof gpt_kinds[0]; i++) {
        if (memcmp(guid, gpt_kinds[i].bytes, 16) == 0) {
            return gpt_kinds[i].name;
        }
    }
    return "unknown";
}

static bool guid_is_zero(const uint8_t *guid) {
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}

/* ---- mbr ------------------------------------------------------------- */

#define MBR_TABLE_AT   446
#define MBR_ENTRY_SIZE 16

static size_t scan_mbr(const uint8_t *sector, struct partition *out,
                       size_t max, bool *is_gpt) {
    size_t found = 0;
    *is_gpt = false;

    for (int i = 0; i < 4 && found < max; i++) {
        const uint8_t *e = sector + MBR_TABLE_AT + i * MBR_ENTRY_SIZE;
        uint8_t type = e[4];
        uint32_t first = rd32(e + 8);
        uint32_t count = rd32(e + 12);

        if (type == 0xee) {
            /* the protective entry a gpt disk carries so that old tools
             * see a full disk rather than an empty one. the real table
             * is elsewhere */
            *is_gpt = true;
            return 0;
        }
        if (type == 0x00 || count == 0) {
            continue;
        }

        memset(&out[found], 0, sizeof out[found]);
        out[found].first_lba = first;
        out[found].sectors = count;
        out[found].mbr_type = type;
        out[found].bootable = (e[0] == 0x80);
        out[found].index = (unsigned)(i + 1);
        out[found].kind = part_mbr_kind(type);
        found++;

        /* an extended partition is a linked list of further tables
         * inside itself. it is listed and not walked: the chain is a
         * different shape of parsing, nothing here makes one, and a
         * half-walked chain is worse than an honest "there is more in
         * here than I read" */
    }
    return found;
}

/* ---- gpt -------------------------------------------------------------- */

#define GPT_SIG_0 0x20494645u       /* "EFI " */
#define GPT_SIG_1 0x54524150u       /* "PART" */

static size_t scan_gpt(part_io read, void *ctx, struct partition *out,
                       size_t max) {
    uint8_t header[PART_SECTOR];
    if (!read(ctx, 1, 1, header)) {
        return 0;
    }
    if (rd32(header) != GPT_SIG_0 || rd32(header + 4) != GPT_SIG_1) {
        return 0;
    }

    uint32_t header_size = rd32(header + 12);
    if (header_size < 92 || header_size > PART_SECTOR) {
        return 0;
    }

    /* the header's own crc is computed with its crc field zeroed, which
     * is the only way a checksum can cover the field that holds it */
    uint32_t claimed = rd32(header + 16);
    uint8_t copy[PART_SECTOR];
    memcpy(copy, header, header_size);
    memset(copy + 16, 0, 4);
    if (part_crc32(copy, header_size) != claimed) {
        return 0;       /* a table known to be corrupt is not followed */
    }

    uint64_t entries_lba = rd64(header + 72);
    uint32_t entry_count = rd32(header + 80);
    uint32_t entry_size = rd32(header + 84);
    uint32_t entries_crc = rd32(header + 88);

    if (entry_size < 128 || entry_size > PART_SECTOR
        || entry_count == 0 || entry_count > 256) {
        return 0;
    }

    /* the array, checksummed whole. one sector at a time, because the
     * whole of it can be thirty-two kilobytes and this kernel reads
     * into a buffer on the stack */
    uint32_t total = entry_count * entry_size;
    uint32_t sectors = (total + PART_SECTOR - 1) / PART_SECTOR;

    uint32_t crc = 0xffffffffu;
    size_t found = 0;
    uint8_t buf[PART_SECTOR];
    uint32_t left = total;

    /* two passes would mean reading it twice, so the crc is accumulated
     * as the entries are parsed and only trusted afterwards -- which is
     * why nothing is written to `out` until the sum comes out right */
    struct partition scratch[PART_MAX];

    for (uint32_t s = 0; s < sectors; s++) {
        if (!read(ctx, entries_lba + s, 1, buf)) {
            return 0;
        }
        uint32_t take = (left > PART_SECTOR) ? PART_SECTOR : left;

        /* fold this sector into the running sum */
        for (uint32_t i = 0; i < take; i++) {
            crc ^= buf[i];
            crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
            crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
        }
        left -= take;

        for (uint32_t at = 0; at + entry_size <= PART_SECTOR
                              && found < PART_MAX; at += entry_size) {
            const uint8_t *e = buf + at;
            if (guid_is_zero(e)) {
                continue;       /* an unused slot, which gpt leaves in place */
            }

            uint64_t first = rd64(e + 32);
            uint64_t last = rd64(e + 40);
            if (last < first) {
                continue;
            }

            memset(&scratch[found], 0, sizeof scratch[found]);
            scratch[found].first_lba = first;
            scratch[found].sectors = last - first + 1;
            scratch[found].kind = gpt_kind(e);
            scratch[found].index = (unsigned)(s * (PART_SECTOR / entry_size)
                                              + at / entry_size + 1);

            /* the name is utf-16, and this console is not. anything
             * outside ascii becomes a dot rather than two bytes of
             * nonsense */
            for (int i = 0; i < PART_NAME_MAX; i++) {
                uint16_t ch = (uint16_t)(e[56 + i * 2]
                                         | ((uint16_t)e[57 + i * 2] << 8));
                if (ch == 0) {
                    scratch[found].name[i] = '\0';
                    break;
                }
                scratch[found].name[i] = (ch >= ' ' && ch < 0x7f)
                                       ? (char)ch : '.';
            }
            scratch[found].name[PART_NAME_MAX] = '\0';
            found++;
        }
    }

    if (~crc != entries_crc) {
        return 0;       /* the entries do not add up. do not follow them */
    }

    if (found > max) {
        found = max;
    }
    for (size_t i = 0; i < found; i++) {
        out[i] = scratch[i];
    }
    return found;
}

/* ---- the whole question --------------------------------------------- */

size_t part_scan(part_io read, void *ctx, struct partition *out, size_t max,
                 enum part_scheme *scheme) {
    *scheme = PART_NONE;
    if (read == NULL || max == 0) {
        return 0;
    }

    uint8_t sector[PART_SECTOR];
    if (!read(ctx, 0, 1, sector)) {
        return 0;
    }

    /* no signature means no table. that is not a failure: plenty of
     * images are one filesystem written straight to sector zero, and
     * the caller falls back to treating the whole drive as one */
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        return 0;
    }

    bool is_gpt = false;
    size_t found = scan_mbr(sector, out, max, &is_gpt);

    if (is_gpt) {
        found = scan_gpt(read, ctx, out, max);
        if (found > 0) {
            *scheme = PART_GPT;
        }
        /* a protective mbr with an unreadable gpt behind it is a disk
         * whose real table is gone. saying "no partitions" is right:
         * the four bytes of protective entry describe nothing */
        return found;
    }

    if (found > 0) {
        *scheme = PART_MBR;
    }
    return found;
}
