/* host-side test for ext2, against a real image.
 *
 * the driver takes its disk as two functions, so here they are pread and
 * pwrite on an image tools/mkext2.py just built. that runs the actual
 * driver over an actual filesystem rather than over a fixture written to
 * match it.
 *
 * there is no e2fsck on this machine, so the usual escape -- "mount it
 * on linux and see" -- is not available. the answer is tools/readext2.py:
 * a reader written from the on-disk layout rather than from either the
 * formatter or this driver, run by the makefile *after* this test has
 * finished scribbling on the image. so what it checks is not what the
 * formatter produced but what the driver *wrote*, which is the part that
 * would otherwise have nobody's opinion on it but its own.
 *
 * that arrangement exists because the alternative caught this project
 * out once already: mkfat.py and the fat32 parser agreed on a cluster
 * count that was wrong, and only a separately written reader found it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

void panic(const char *fmt, ...) {
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "fs/ext2.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* ---- the disk, which here is a file ---- */

static bool img_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    int fd = *(int *)ctx;
    ssize_t n = pread(fd, buf, (size_t)count * 512, (off_t)(lba * 512));
    return n == (ssize_t)(count * 512);
}

static bool img_write(void *ctx, uint64_t lba, uint32_t count,
                      const void *buf) {
    int fd = *(int *)ctx;
    ssize_t n = pwrite(fd, buf, (size_t)count * 512, (off_t)(lba * 512));
    return n == (ssize_t)(count * 512);
}

/* a clock that does not move, so what should have been written down is
 * exactly knowable rather than merely plausible. 1st march 2026,
 * 14:30:45 utc */
#define FIXED_TIME 1772375445u
static uint32_t fixed_clock(void) { return FIXED_TIME; }

static char *slurp(struct ext2 *fs, struct ext2_file *f, uint64_t *out_len) {
    char *buf = malloc(f->size + 1);
    int64_t n = ext2_read(fs, f, 0, buf, f->size);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (out_len) *out_len = (uint64_t)n;
    return buf;
}

int main(void) {
    /* ---- the arithmetic, which needs no disk at all ----
     *
     * ext2 keeps seconds since 1970 and everything above wants a date.
     * a leap year rule is exactly the kind of thing that is correct for
     * four years, so it is checked against dates somebody can verify */
    {
        struct fat32_time t;

        ext2_unix_to_time(0, &t);
        CHECK(t.year == 1970 && t.month == 1 && t.day == 1,
              "zero is the first of january 1970");
        CHECK(t.hour == 0 && t.minute == 0 && t.second == 0, "at midnight");

        ext2_unix_to_time(86399, &t);
        CHECK(t.year == 1970 && t.month == 1 && t.day == 1
              && t.hour == 23 && t.minute == 59 && t.second == 59,
              "and one second before the next day is still that day");

        ext2_unix_to_time(86400, &t);
        CHECK(t.day == 2, "one more and it is the second");

        /* 2000 was a leap year, 1900 was not, and the difference is the
         * rule everybody gets wrong */
        ext2_unix_to_time(951782400u, &t);      /* 29 feb 2000 00:00 utc */
        CHECK(t.year == 2000 && t.month == 2 && t.day == 29,
              "2000 had a 29th of february, which the four-hundred rule "
              "is the only reason for");

        ext2_unix_to_time(1709208000u, &t);     /* 29 feb 2024 12:00 utc */
        CHECK(t.year == 2024 && t.month == 2 && t.day == 29 && t.hour == 12,
              "and so did 2024, for the ordinary reason");

        ext2_unix_to_time(FIXED_TIME, &t);
        CHECK(t.year == 2026 && t.month == 3 && t.day == 1,
              "the clock this test uses is the first of march 2026");
        CHECK(t.hour == 14 && t.minute == 30 && t.second == 45, "at half two");

        /* and back again, for a few thousand days spread over a century */
        bool round_trips = true;
        for (uint32_t s = 0; s < 3000000000u; s += 999983) {
            ext2_unix_to_time(s, &t);
            if (ext2_time_to_unix(&t) != s) {
                round_trips = false;
                break;
            }
        }
        CHECK(round_trips,
              "and three thousand dates across a century convert both ways "
              "and come back to the same second");
    }

    /* ---- and now the disk ---- */

    int fd = open("bin/tests/ext2.img", O_RDWR);
    if (fd < 0) {
        printf("FAIL: no bin/tests/ext2.img -- `make ext2-image`\n");
        return 1;
    }

    struct ext2 fs;
    CHECK(ext2_mount(&fs, img_read, img_write, &fd), "the image mounts");
    if (!fs.mounted) {
        return 1;
    }
    ext2_set_clock(&fs, fixed_clock);

    CHECK(fs.block_size == 1024, "with 1 KiB blocks");
    CHECK(fs.inode_size == 128, "and 128-byte inodes");
    CHECK(strcmp(fs.label, "tinyos") == 0, "and the label it was given");

    /* ---- reading what the formatter put there ---- */

    struct ext2_file f;
    CHECK(ext2_lookup(&fs, "hello.txt", &f), "a file is found");
    CHECK(!f.is_dir, "and is not a directory");
    CHECK(f.size > 0, "and has a size");
    CHECK(f.mode & 0400, "and permissions, which fat had nowhere to keep");

    uint64_t len = 0;
    char *text = slurp(&fs, &f, &len);
    CHECK(text != NULL && len == f.size, "it reads");
    free(text);

    CHECK(ext2_lookup(&fs, "notes/deep.txt", &f), "a nested path resolves");
    CHECK(ext2_lookup(&fs, "/notes/deep.txt", &f), "with or without a slash");
    CHECK(!ext2_lookup(&fs, "notes/nothing", &f), "and a name that is not there is not found");
    CHECK(!ext2_lookup(&fs, "hello.txt/deeper", &f),
          "and a file is not a directory to walk through");

    /* the whole point of the version: an inode is the identity, and a
     * name is a separate thing pointing at it */
    struct ext2_file again;
    CHECK(ext2_lookup(&fs, "hello.txt", &again) && again.ino == f.ino
          ? false : true, "two different names have two different inodes");

    /* ---- a file big enough to need indirect blocks ----
     *
     * twelve blocks is twelve kilobytes, and past that the block
     * numbers stop being in the inode. this is where a driver that
     * only ever tested small files falls over */
    {
        CHECK(ext2_lookup(&fs, "indirect.bin", &f), "a 20k file is found");
        CHECK(f.size == 20000, "with its size");
        text = slurp(&fs, &f, &len);
        CHECK(text != NULL && len == 20000, "and reads whole");

        bool right = (text != NULL);
        for (uint64_t i = 0; right && i < 20000; i++) {
            if ((uint8_t)text[i] != (uint8_t)((i * 7 + 3) & 0xff)) {
                right = false;
            }
        }
        CHECK(right, "every byte of it, across the indirect block");
        free(text);
    }
    {
        CHECK(ext2_lookup(&fs, "double.bin", &f), "a 400k file is found");
        CHECK(f.size == 400000, "with its size");
        text = slurp(&fs, &f, &len);
        CHECK(text != NULL && len == 400000, "and reads whole");

        bool right = (text != NULL);
        for (uint64_t i = 0; right && i < 400000; i++) {
            if ((uint8_t)text[i] != (uint8_t)((i * 13 + 5) & 0xff)) {
                right = false;
            }
        }
        CHECK(right,
              "every byte of that too, which needs the double indirect "
              "block and is where a driver tested only on small files "
              "falls over");
        free(text);
    }

    /* reading from the middle, which is what a descriptor does */
    CHECK(ext2_lookup(&fs, "double.bin", &f), "the big file again");
    {
        uint8_t chunk[300];
        CHECK(ext2_read(&fs, &f, 300000, chunk, sizeof chunk)
              == (int64_t)sizeof chunk, "a read from the middle works");
        bool right = true;
        for (uint64_t i = 0; i < sizeof chunk; i++) {
            if (chunk[i] != (uint8_t)(((300000 + i) * 13 + 5) & 0xff)) {
                right = false;
            }
        }
        CHECK(right, "and lands exactly where it was asked to");

        CHECK(ext2_read(&fs, &f, 400000, chunk, 10) == 0,
              "reading at the end gives nothing, which is not an error");
        CHECK(ext2_read(&fs, &f, 399995, chunk, 100) == 5,
              "and a read past the end is shortened to what is there");
    }

    /* ---- symlinks, which fat cannot express at all ---- */

    {
        char target[256];

        CHECK(ext2_lookup_nofollow(&fs, "sub/link", &f), "a symlink is found");
        CHECK(f.is_symlink, "and says it is one");
        CHECK(ext2_readlink(&fs, &f, target, sizeof target), "it can be read");
        CHECK(strcmp(target, "../indirect.bin") == 0, "and says where it points");

        /* and following it gets the file rather than the link */
        struct ext2_file through;
        CHECK(ext2_lookup(&fs, "sub/link", &through), "following it works");
        CHECK(!through.is_symlink, "and arrives at something that is not a link");
        CHECK(through.size == 20000, "which is the file it named");

        /* a long target does not fit in the inode and lives in a block
         * instead. both kinds have to work, and the short one is the
         * only place in ext2 where the block pointers are not block
         * pointers */
        CHECK(ext2_lookup_nofollow(&fs, "slowlink", &f), "a long symlink is found");
        CHECK(ext2_readlink(&fs, &f, target, sizeof target), "and read");
        CHECK(strlen(target) == 67,
              "with all of it, which for a target this long means out of a "
              "block rather than out of the inode");
    }

    /* ---- listing ---- */

    {
        int count = 0;
        bool saw_notes = false, saw_hello = false, saw_dot = false;
        struct ext2_file e;
        for (size_t i = 0; ext2_readdir(&fs, 0, i, &e); i++) {
            if (strcmp(e.name, "notes") == 0) { saw_notes = true; }
            if (strcmp(e.name, "hello.txt") == 0) { saw_hello = true; }
            if (e.name[0] == '.') { saw_dot = true; }
            count++;
        }
        CHECK(count > 0, "the root lists");
        CHECK(saw_notes && saw_hello, "with what is in it");
        CHECK(!saw_dot,
              "and without . or .., which nothing above this has any use "
              "for");
    }

    /* ---- writing ---- */

    struct ext2_file made;
    CHECK(ext2_create(&fs, "fresh.txt", 0644, 0, 0, &made), "a file is made");
    CHECK(made.ino != 0, "with an inode of its own");
    CHECK((made.mode & 0777) == 0644, "and the mode it was asked for");

    CHECK(ext2_write(&fs, &made, 0, "hee-ho\n", 7) == 7, "and written to");
    CHECK(ext2_lookup(&fs, "fresh.txt", &f), "and found again");
    CHECK(f.size == 7, "with the size it was given");
    text = slurp(&fs, &f, &len);
    CHECK(text && strcmp(text, "hee-ho\n") == 0, "and the bytes");
    free(text);

    /* its timestamp is the one the fixed clock said, to the second --
     * ext2 keeps whole seconds rather than fat's twos */
    CHECK(f.modified.year == 2026 && f.modified.month == 3
          && f.modified.day == 1, "stamped with the date it was written");
    CHECK(f.modified.hour == 14 && f.modified.minute == 30
          && f.modified.second == 45,
          "to the second, which fat could not manage -- it kept them in "
          "twos");

    /* a write past twelve blocks, so the driver has to allocate an
     * indirect block of its own rather than only read one */
    {
        struct ext2_file big;
        CHECK(ext2_create(&fs, "grown.bin", 0644, 0, 0, &big), "a file to grow");

        static uint8_t lump[40000];
        for (size_t i = 0; i < sizeof lump; i++) {
            lump[i] = (uint8_t)((i * 31 + 17) & 0xff);
        }
        CHECK(ext2_write(&fs, &big, 0, lump, sizeof lump)
              == (int64_t)sizeof lump,
              "forty kilobytes are written");

        CHECK(ext2_lookup(&fs, "grown.bin", &f), "and it is found");
        CHECK(f.size == sizeof lump, "at the right size");

        text = slurp(&fs, &f, &len);
        bool right = (text != NULL && len == sizeof lump);
        for (uint64_t i = 0; right && i < len; i++) {
            if ((uint8_t)text[i] != lump[i]) {
                right = false;
            }
        }
        CHECK(right,
              "and reads back byte for byte -- which means the driver "
              "allocated the indirect block as well as read one");
        free(text);
    }

    /* ---- ownership and permissions, which is the point of all this ---- */

    CHECK(ext2_chmod(&fs, "fresh.txt", 0600), "a file can be chmodded");
    CHECK(ext2_lookup(&fs, "fresh.txt", &f), "and found");
    CHECK((f.mode & 0777) == 0600, "with the new mode");
    CHECK((f.mode & EXT2_S_IFMT) == EXT2_S_IFREG,
          "and it is still a regular file -- chmod changes permissions, "
          "not what something is");

    CHECK(ext2_chown(&fs, "fresh.txt", 1000, 1000), "and chowned");
    CHECK(ext2_lookup(&fs, "fresh.txt", &f), "and found");
    CHECK(f.uid == 1000 && f.gid == 1000, "with the new owner");
    CHECK((f.mode & 0777) == 0600, "and the mode it already had");

    /* ---- making and unmaking ---- */

    CHECK(ext2_mkdir(&fs, "made", 0755, 0, 0), "a directory is made");
    CHECK(ext2_lookup(&fs, "made", &f), "and found");
    CHECK(f.is_dir, "and is one");
    CHECK(f.links == 2, "with the two links every new directory has");

    CHECK(ext2_create(&fs, "made/inside.txt", 0644, 0, 0, &made),
          "and something can be put in it");
    CHECK(ext2_write(&fs, &made, 0, "deeper\n", 7) == 7, "and written");
    CHECK(ext2_lookup(&fs, "made/inside.txt", &f), "and found by path");

    CHECK(!ext2_rmdir(&fs, "made"), "a directory with something in it stays");
    CHECK(ext2_unlink(&fs, "made/inside.txt"), "until it is emptied");
    CHECK(ext2_rmdir(&fs, "made"), "and then it goes");
    CHECK(!ext2_lookup(&fs, "made", &f), "and is gone");

    CHECK(!ext2_unlink(&fs, "notes"), "a directory is not a file to unlink");
    CHECK(!ext2_rmdir(&fs, "hello.txt"), "nor a file a directory to remove");

    /* a symlink made by the driver rather than the formatter */
    CHECK(ext2_symlink(&fs, "shortcut", "notes/deep.txt", 0, 0),
          "a symlink can be made");
    {
        char target[256];
        CHECK(ext2_lookup_nofollow(&fs, "shortcut", &f), "and found");
        CHECK(f.is_symlink, "and is one");
        CHECK(ext2_readlink(&fs, &f, target, sizeof target)
              && strcmp(target, "notes/deep.txt") == 0, "pointing where it was told");
        CHECK(ext2_lookup(&fs, "shortcut", &f) && !f.is_symlink,
              "and following it arrives somewhere real");
    }

    /* ---- renaming, which moves a name and not a file ---- */

    CHECK(ext2_create(&fs, "before.txt", 0644, 0, 0, &made), "a file to rename");
    CHECK(ext2_write(&fs, &made, 0, "same bytes\n", 11) == 11, "with contents");
    CHECK(ext2_lookup(&fs, "before.txt", &f), "found");
    uint32_t same_ino = f.ino;

    CHECK(ext2_rename(&fs, "before.txt", "after.txt"), "it renames");
    CHECK(!ext2_lookup(&fs, "before.txt", &f), "the old name is gone");
    CHECK(ext2_lookup(&fs, "after.txt", &f), "the new one is there");
    CHECK(f.ino == same_ino,
          "and it is the very same inode -- a rename moves a name, and "
          "nothing else moved at all");

    CHECK(ext2_rename(&fs, "after.txt", "notes/moved.txt"),
          "and it can move to another directory");
    CHECK(ext2_lookup(&fs, "notes/moved.txt", &f), "arriving there");
    CHECK(f.ino == same_ino, "still the same inode");
    CHECK(!ext2_rename(&fs, "notes/moved.txt", "hello.txt"),
          "but not onto a name already taken");

    /* ---- what a read-only mount refuses ---- */

    {
        struct ext2 ro;
        CHECK(ext2_mount(&ro, img_read, NULL, &fd), "a read-only mount works");
        CHECK(ext2_lookup(&ro, "hello.txt", &f), "and reads");

        struct ext2_file nope;
        CHECK(!ext2_create(&ro, "no.txt", 0644, 0, 0, &nope), "but makes nothing");
        CHECK(!ext2_mkdir(&ro, "nodir", 0755, 0, 0), "no directories");
        CHECK(!ext2_unlink(&ro, "hello.txt"), "removes nothing");
        CHECK(!ext2_chmod(&ro, "hello.txt", 0600), "changes no permissions");
        CHECK(!ext2_chown(&ro, "hello.txt", 1, 1), "and no owners");
        CHECK(!ext2_symlink(&ro, "l", "hello.txt", 0, 0), "and links nothing");
    }

    /* ---- and that it all survives being mounted again ---- */

    {
        struct ext2 remount;
        CHECK(ext2_mount(&remount, img_read, img_write, &fd),
              "the image mounts again");
        CHECK(ext2_lookup(&remount, "fresh.txt", &f), "the new file is there");
        CHECK(f.uid == 1000 && (f.mode & 0777) == 0600,
              "with its owner and mode -- which is the whole reason this "
              "filesystem exists");
        CHECK(ext2_lookup(&remount, "notes/moved.txt", &f), "and the moved one");
        CHECK(ext2_lookup_nofollow(&remount, "shortcut", &f) && f.is_symlink,
              "and the symlink is still a symlink");
        CHECK(!ext2_lookup(&remount, "made", &f), "and what was removed is gone");

        uint64_t used = 0, total = 0;
        CHECK(ext2_usage(&remount, &used, &total), "usage can be counted");
        CHECK(total > used && used > 0, "and is a sensible fraction of it");
    }

    close(fd);

    if (failures == 0) printf("all good\n");
    return failures;
}
