/* host-side test for partition tables.
 *
 * two halves, and they check different things.
 *
 * the tables built here by hand are for the cases that matter and never
 * happen on a healthy disk: a table with no signature, a gpt whose
 * checksum does not add up, a protective mbr with nothing behind it. a
 * parser is judged by what it refuses, because following a corrupt
 * table means mounting a filesystem at an address nobody chose.
 *
 * and then the real images that tools/mkdisk.py builds, read by this
 * parser. the two were written from the specification at different
 * times in different languages, which is the same second-opinion
 * argument that tools/readext2.py makes -- with the crc32 vector below
 * anchoring both of them to something neither author invented. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include "drivers/part.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* ---- a disk in memory, which the test writes tables into ---- */

#define DISK_SECTORS 256
static uint8_t disk[DISK_SECTORS * PART_SECTOR];
static bool disk_refuses;

static bool mem_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (disk_refuses) return false;
    if (lba + count > DISK_SECTORS) return false;
    memcpy(buf, &disk[lba * PART_SECTOR], (size_t)count * PART_SECTOR);
    return true;
}

/* ---- a real image on the filesystem ---- */

static int img_fd = -1;
static bool file_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    ssize_t n = pread(img_fd, buf, (size_t)count * PART_SECTOR,
                      (off_t)(lba * PART_SECTOR));
    return n == (ssize_t)(count * PART_SECTOR);
}

/* ---- building tables by hand ---- */

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static void blank_disk(void) {
    memset(disk, 0, sizeof disk);
    disk_refuses = false;
}

static void mbr_entry(int slot, uint8_t type, uint32_t first, uint32_t count,
                      bool boot) {
    uint8_t *e = disk + 446 + slot * 16;
    e[0] = boot ? 0x80 : 0x00;
    e[4] = type;
    wr32(e + 8, first);
    wr32(e + 12, count);
}

static void sign_mbr(void) {
    disk[510] = 0x55;
    disk[511] = 0xaa;
}

/* a gpt with `count` entries, written at sector 1 and 2 onwards */
static void build_gpt(int count, bool break_header_crc, bool break_entry_crc) {
    uint32_t entry_size = 128;
    uint32_t entry_count = 8;
    uint32_t entries_bytes = entry_count * entry_size;

    uint8_t *entries = disk + 2 * PART_SECTOR;
    memset(entries, 0, entries_bytes);

    for (int i = 0; i < count; i++) {
        uint8_t *e = entries + i * entry_size;
        /* the linux filesystem type guid */
        static const uint8_t linux_guid[16] = {
            0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
            0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
        memcpy(e, linux_guid, 16);
        memset(e + 16, 0x11 + i, 16);
        wr64(e + 32, 34 + (uint64_t)i * 20);
        wr64(e + 40, 34 + (uint64_t)i * 20 + 19);

        /* a name, in utf-16 */
        const char *name = (i == 0) ? "root" : "spare";
        for (int c = 0; name[c] != '\0'; c++) {
            e[56 + c * 2] = (uint8_t)name[c];
            e[57 + c * 2] = 0;
        }
    }

    uint32_t entries_crc = part_crc32(entries, entries_bytes);
    if (break_entry_crc) {
        entries_crc ^= 1;
    }

    uint8_t *h = disk + PART_SECTOR;
    memset(h, 0, PART_SECTOR);
    memcpy(h, "EFI PART", 8);
    wr32(h + 8, 0x00010000);
    wr32(h + 12, 92);
    wr32(h + 16, 0);
    wr64(h + 24, 1);
    wr64(h + 32, DISK_SECTORS - 1);
    wr64(h + 40, 34);
    wr64(h + 48, DISK_SECTORS - 34);
    wr64(h + 72, 2);
    wr32(h + 80, entry_count);
    wr32(h + 84, entry_size);
    wr32(h + 88, entries_crc);
    wr32(h + 16, part_crc32(h, 92));

    if (break_header_crc) {
        h[16] ^= 1;
    }

    /* the protective entry that makes this a gpt disk */
    mbr_entry(0, 0xee, 1, DISK_SECTORS - 1, false);
    sign_mbr();
}

int main(void) {
    struct partition parts[PART_MAX];
    enum part_scheme scheme;
    size_t n;

    /* ---- crc32, against a number neither author invented ----
     *
     * this one vector is what anchors the gpt half of this to the
     * outside world. everything else here is my table read by my
     * parser; "123456789" hashing to 0xcbf43926 is the reflected crc32
     * everybody means, and if this line fails then every checksum below
     * agrees with itself and with nothing else */
    CHECK(part_crc32("123456789", 9) == 0xcbf43926u,
          "crc32 of \"123456789\" is 0xcbf43926, as it is everywhere");
    CHECK(part_crc32("", 0) == 0, "and of nothing is nothing");
    CHECK(part_crc32("a", 1) == 0xe8b7be43u, "and of \"a\" is 0xe8b7be43");

    /* ---- no table at all ---- */

    blank_disk();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0 && scheme == PART_NONE,
          "a disk with no signature has no partitions");

    /* that is not a failure, and the difference matters: plenty of
     * images are one filesystem written straight to sector zero, and
     * the caller has to be able to tell "no table" from "bad table" */

    blank_disk();
    disk_refuses = true;
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0 && scheme == PART_NONE, "and a drive that will not read has none");

    /* ---- mbr ---- */

    blank_disk();
    mbr_entry(0, 0x83, 2048, 8192, true);
    mbr_entry(1, 0x0c, 10240, 4096, false);
    sign_mbr();

    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2 && scheme == PART_MBR, "an mbr with two entries reads as two");
    CHECK(parts[0].first_lba == 2048 && parts[0].sectors == 8192,
          "with the first one's place and size");
    CHECK(parts[0].index == 1, "numbered from one, the way everybody does");
    CHECK(parts[0].bootable, "and the active flag");
    CHECK(strcmp(parts[0].kind, "linux") == 0, "and what the type byte means");
    CHECK(parts[1].first_lba == 10240 && !parts[1].bootable,
          "and the second one too");
    CHECK(strcmp(parts[1].kind, "fat32") == 0, "with its own type");
    CHECK(parts[0].name[0] == '\0',
          "and no name, because mbr has nowhere to keep one -- an empty "
          "string rather than an invented one");

    /* empty slots in the middle are skipped, and the ones after them
     * are still found. a table is four fixed slots, not a list */
    blank_disk();
    mbr_entry(0, 0x00, 0, 0, false);
    mbr_entry(1, 0x83, 100, 200, false);
    mbr_entry(3, 0x83, 500, 600, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2, "empty slots are skipped rather than ending the table");
    CHECK(parts[0].first_lba == 100 && parts[1].first_lba == 500,
          "and what is after them is still found");
    CHECK(parts[0].index == 2 && parts[1].index == 4,
          "keeping the slot numbers they really occupy, since that is what "
          "anybody else calls them");

    /* an entry with a length of zero describes nothing */
    blank_disk();
    mbr_entry(0, 0x83, 2048, 0, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "an entry of zero length is not a partition");

    /* extended partitions are listed, not walked */
    blank_disk();
    mbr_entry(0, 0x05, 2048, 8192, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 1 && strcmp(parts[0].kind, "extended") == 0,
          "an extended partition is listed as one");

    /* ---- gpt ---- */

    blank_disk();
    build_gpt(2, false, false);
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2 && scheme == PART_GPT, "a gpt disk reads as gpt");
    CHECK(parts[0].first_lba == 34 && parts[0].sectors == 20,
          "with 64-bit addresses, and a length worked out from the last "
          "sector rather than stored");
    CHECK(strcmp(parts[0].name, "root") == 0,
          "and the name, which is the thing mbr could not do");
    CHECK(strcmp(parts[1].name, "spare") == 0, "for each of them");
    CHECK(strcmp(parts[0].kind, "linux") == 0,
          "and the type, read from a guid rather than a byte");
    CHECK(parts[0].mbr_type == 0,
          "with no mbr type, because there is no mbr entry behind it");

    /* the protective mbr must not be reported as a partition. it spans
     * the whole disk, so following it would mount the table itself */
    bool any_protective = false;
    for (size_t i = 0; i < n; i++) {
        if (parts[i].first_lba == 1) {
            any_protective = true;
        }
    }
    CHECK(!any_protective,
          "and the protective mbr entry is not among them -- it covers the "
          "whole disk, so following it would mount the table");

    /* ---- and what it refuses ----
     *
     * this is the half that matters. a checksum that does not add up
     * means a table that cannot be trusted, and a table that cannot be
     * trusted must not be followed -- mounting a filesystem at an
     * address nobody chose is worse than mounting nothing */

    blank_disk();
    build_gpt(2, true, false);
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "a gpt header whose crc is wrong is refused");
    CHECK(scheme == PART_NONE, "and reported as no table rather than a bad one");

    blank_disk();
    build_gpt(2, false, true);
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0,
          "and so is one whose *entries* do not add up, even though its "
          "header does");

    /* a protective mbr with nothing behind it. the four bytes of
     * protective entry describe the whole disk and nothing else, so
     * there is genuinely nothing to report */
    blank_disk();
    mbr_entry(0, 0xee, 1, DISK_SECTORS - 1, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "a protective mbr with no gpt behind it has no partitions");

    /* nonsense in the header's own fields */
    blank_disk();
    build_gpt(1, false, false);
    disk[PART_SECTOR + 84] = 0;         /* entry size of zero */
    wr32(disk + PART_SECTOR + 16, 0);
    wr32(disk + PART_SECTOR + 16, part_crc32(disk + PART_SECTOR, 92));
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "an entry size of zero is refused rather than divided by");

    /* an entry whose last sector is before its first */
    blank_disk();
    build_gpt(1, false, false);
    {
        uint8_t *e = disk + 2 * PART_SECTOR;
        wr64(e + 40, 10);               /* last, now below first */
        wr32(disk + PART_SECTOR + 88, part_crc32(disk + 2 * PART_SECTOR, 8 * 128));
        wr32(disk + PART_SECTOR + 16, 0);
        wr32(disk + PART_SECTOR + 16, part_crc32(disk + PART_SECTOR, 92));
    }
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "and an entry that ends before it starts is skipped");

    /* ---- the real images, built by a tool written separately ---- */

    for (int which = 0; which < 2; which++) {
        const char *path = which ? "bin/tests/gpt.img" : "bin/tests/mbr.img";
        img_fd = open(path, O_RDONLY);
        if (img_fd < 0) {
            printf("FAIL: no %s -- `make part-images`\n", path);
            failures++;
            continue;
        }

        n = part_scan(file_read, NULL, parts, PART_MAX, &scheme);
        CHECK(n == 2, which ? "the real gpt image has two partitions"
                            : "the real mbr image has two partitions");
        CHECK(scheme == (which ? PART_GPT : PART_MBR),
              "of the scheme it was built with");
        if (n == 2) {
            CHECK(parts[0].first_lba == 2048,
                  "the first aligned to a mebibyte, as every tool has since "
                  "disks stopped having real cylinders");
            CHECK(parts[1].first_lba > parts[0].first_lba
                  && parts[1].first_lba >= parts[0].first_lba + parts[0].sectors,
                  "and the second after it, not overlapping");
        }
        close(img_fd);
        img_fd = -1;
    }

    if (failures == 0) printf("all good\n");
    return failures;
}
