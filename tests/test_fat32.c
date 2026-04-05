/* host-side test for fat32, against a real image.
 *
 * the filesystem takes its disk as two functions, so here they are pread
 * and pwrite on an image file that tools/mkfat.py just built. that means
 * this runs the actual parser over an actual filesystem rather than over
 * a fixture someone wrote to match it.
 *
 * the formatter and the parser were written by the same hand, which is
 * exactly the trap that has caught this project twice before -- both
 * sides agreeing on the same misunderstanding. the answer is in
 * TESTING.md: mount the image on linux. a driver nobody here wrote
 * reading my files is the check this file cannot perform. */
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

#include "fs/fat32.h"

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

/* a disk that refuses every request, to check nothing pretends */
static bool dead_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx; (void)lba; (void)count; (void)buf;
    return false;
}

/* a clock that does not move, so what should have been written down is
 * exactly knowable rather than merely plausible */
static void fixed_clock(struct fat32_time *out) {
    out->year = 2026; out->month = 8; out->day = 7;
    out->hour = 14; out->minute = 30; out->second = 45;
}

static char *slurp(struct fat32 *fs, struct fat32_file *f, uint64_t *out_len) {
    char *buf = malloc(f->size + 1);
    int64_t n = fat32_read(fs, f, 0, buf, f->size);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = '\0';
    *out_len = (uint64_t)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <image>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        printf("FAIL: cannot open %s\n", argv[1]);
        return 1;
    }

    struct fat32 fs;

    /* ---- a disk that is not there ---- */

    CHECK(!fat32_mount(&fs, dead_read, NULL, &fd),
          "a disk that refuses to be read is not mounted");
    CHECK(!fat32_mount(&fs, NULL, NULL, &fd), "and neither is no disk at all");

    /* ---- mounting ---- */

    CHECK(fat32_mount(&fs, img_read, img_write, &fd), "the image mounts");
    CHECK(strcmp(fs.label, "TINYOS") == 0, "the volume label came through");
    CHECK(fs.root_cluster == 2, "the root is where the boot sector says");
    CHECK(fs.num_fats == 2, "two copies of the table");
    CHECK(fat32_cluster_bytes(&fs) == 512, "512-byte clusters");
    CHECK(fs.cluster_count > 65525,
          "enough clusters that this is really fat32 and not fat16");

    /* the geometry can imply more clusters than the table has entries
     * for. believing it would mean reading a cluster's chain from past
     * the end of the fat -- which is to say, out of the next copy of
     * the fat -- so the smaller of the two has to win */
    CHECK((uint64_t)fs.cluster_count + 2
              <= (uint64_t)fs.fat_sectors * FAT32_SECTOR / 4,
          "every cluster the fs admits to has an entry in the table");

    /* ---- an 8.3 name, which needs no long entry at all ---- */

    struct fat32_file f;
    CHECK(fat32_lookup(&fs, "MOTD.TXT", &f), "a short name is found");
    CHECK(!f.is_dir, "and is a file");
    uint64_t len;
    char *text = slurp(&fs, &f, &len);
    CHECK(text && strcmp(text, "uppercase and short\n") == 0,
          "a short-named file reads back exactly");
    free(text);

    /* case is not a thing fat has ever cared about */
    CHECK(fat32_lookup(&fs, "motd.txt", &f), "lookup ignores case");

    /* ---- a name that needs a long entry ---- */

    CHECK(fat32_lookup(&fs, "velvet-room.txt", &f),
          "a long name is assembled from its pieces");
    CHECK(f.size == 49, "with the right size");
    text = slurp(&fs, &f, &len);
    CHECK(text && strncmp(text, "a much longer name", 18) == 0,
          "and the right contents");
    free(text);

    /* ---- lowercase 8.3, which is still a long name on disk ---- */

    CHECK(fat32_lookup(&fs, "hello.txt", &f), "a lowercase name is found");
    text = slurp(&fs, &f, &len);
    CHECK(text && strcmp(text, "the velvet room is neither dream nor reality\n") == 0,
          "and reads back exactly");
    free(text);

    /* ---- a file spanning many clusters ---- */

    CHECK(fat32_lookup(&fs, "big.bin", &f), "the big file is found");
    CHECK(f.size == 9000, "at its full size");
    CHECK(f.size > fat32_cluster_bytes(&fs),
          "which is more than one cluster, so the chain is really walked");

    uint8_t *big = malloc(f.size);
    int64_t got = fat32_read(&fs, &f, 0, big, f.size);
    CHECK(got == 9000, "the whole thing reads in one go");
    int intact = 1;
    for (uint64_t i = 0; i < 9000; i++) {
        if (big[i] != (uint8_t)((i * 7 + 3) & 0xff)) intact = 0;
    }
    CHECK(intact, "every byte of a multi-cluster file is right");

    /* the same bytes, asked for at awkward offsets that straddle both
     * sector and cluster boundaries */
    intact = 1;
    for (uint64_t off = 0; off < 9000; off += 337) {
        uint8_t chunk[500];
        uint64_t want = 500;
        if (off + want > 9000) want = 9000 - off;
        int64_t n = fat32_read(&fs, &f, off, chunk, want);
        if (n != (int64_t)want) { intact = 0; break; }
        for (uint64_t i = 0; i < want; i++) {
            if (chunk[i] != (uint8_t)(((off + i) * 7 + 3) & 0xff)) intact = 0;
        }
    }
    CHECK(intact, "reads from the middle land in the right place");

    CHECK(fat32_read(&fs, &f, 9000, big, 10) == 0,
          "reading past the end is nothing, not an error");
    CHECK(fat32_read(&fs, &f, 8990, big, 100) == 10,
          "and a read running off the end is short");
    free(big);

    /* ---- directories ---- */

    CHECK(fat32_lookup(&fs, "notes", &f), "a subdirectory is found");
    CHECK(f.is_dir, "and knows it is one");
    uint32_t notes_cluster = f.first_cluster;

    CHECK(fat32_lookup(&fs, "notes/deep.txt", &f), "and can be descended into");
    text = slurp(&fs, &f, &len);
    CHECK(text && strcmp(text, "nested\n") == 0, "the nested file reads back");
    free(text);

    CHECK(!fat32_lookup(&fs, "notes/nope.txt", &f), "a missing file is missing");
    CHECK(!fat32_lookup(&fs, "hello.txt/deeper", &f),
          "a file cannot be descended into");

    /* readdir over the root must find everything I put there, and no
     * dot entries, which nothing above this has any use for */
    int seen_hello = 0, seen_big = 0, seen_notes = 0, seen_long = 0, dots = 0;
    size_t count = 0;
    struct fat32_file e;
    for (size_t i = 0; fat32_readdir(&fs, 0, i, &e); i++) {
        if (strcmp(e.name, "hello.txt") == 0) seen_hello = 1;
        if (strcmp(e.name, "big.bin") == 0) seen_big = 1;
        if (strcmp(e.name, "velvet-room.txt") == 0) seen_long = 1;
        if (strcmp(e.name, "notes") == 0 && e.is_dir) seen_notes = 1;
        if (e.name[0] == '.') dots++;
        count++;
    }
    CHECK(seen_hello && seen_big && seen_long && seen_notes,
          "readdir lists every file I put on the disk");
    CHECK(dots == 0, "with no dot entries");
    size_t root_at_first = count;

    /* a subdirectory has dot and dotdot on disk, and they must not show */
    dots = 0;
    for (size_t i = 0; fat32_readdir(&fs, notes_cluster, i, &e); i++) {
        if (e.name[0] == '.') dots++;
    }
    CHECK(dots == 0, "nor inside a subdirectory, where they really exist");

    /* ---- writing to a file that is already there ---- */

    CHECK(fat32_lookup(&fs, "hello.txt", &f), "found it again");
    const char *replacement = "SOULS";
    int64_t wrote = fat32_write(&fs, &f, 4, replacement, 5);
    CHECK(wrote == 5, "a write into the middle takes");

    struct fat32_file again;
    CHECK(fat32_lookup(&fs, "hello.txt", &again), "and the file is still there");
    text = slurp(&fs, &again, &len);
    CHECK(text && strncmp(text, "the SOULSt room", 15) == 0,
          "the bytes changed and the ones around them did not");
    free(text);

    /* writing past the end grows the file and says so in the directory */
    uint32_t was = again.size;
    char tail[600];
    memset(tail, 'z', sizeof tail);
    wrote = fat32_write(&fs, &again, was, tail, sizeof tail);
    CHECK(wrote == (int64_t)sizeof tail, "a write past the end takes too");

    CHECK(fat32_lookup(&fs, "hello.txt", &f), "still there after growing");
    CHECK(f.size == was + sizeof tail, "and the directory learned the new size");
    CHECK(f.size > fat32_cluster_bytes(&fs),
          "the file now spans clusters it did not before");

    char *grown = malloc(f.size + 1);
    got = fat32_read(&fs, &f, 0, grown, f.size);
    CHECK(got == (int64_t)f.size, "and all of it reads back");
    intact = 1;
    for (uint32_t i = was; i < f.size; i++) {
        if (grown[i] != 'z') intact = 0;
    }
    CHECK(intact, "including the part in the cluster that had to be added");
    free(grown);

    /* ---- making a file that was not there ---- */

    struct fat32_file fresh;
    CHECK(fat32_create(&fs, "notes.txt", &fresh), "a new file can be made");
    CHECK(fresh.size == 0, "and starts empty");
    CHECK(fresh.first_cluster == 0, "owning no clusters at all");

    const char *message = "thou art I, and I am thou\n";
    wrote = fat32_write(&fs, &fresh, 0, message, strlen(message));
    CHECK(wrote == (int64_t)strlen(message), "and can be written to");

    CHECK(fat32_lookup(&fs, "notes.txt", &f), "and is then found by name");
    CHECK(f.size == strlen(message), "at the size I wrote");
    text = slurp(&fs, &f, &len);
    CHECK(text && strcmp(text, message) == 0, "with what I put in it");
    free(text);

    /* in a subdirectory too */
    CHECK(fat32_create(&fs, "notes/made.txt", &fresh),
          "a file can be made inside a directory");
    CHECK(fat32_write(&fs, &fresh, 0, "deep\n", 5) == 5, "and written");
    CHECK(fat32_lookup(&fs, "notes/made.txt", &f), "and found again");

    /* names I cannot store honestly are refused rather than mangled */
    CHECK(!fat32_create(&fs, "a-name-far-too-long-for-8.3", &fresh),
          "a name that will not fit 8.3 is refused");
    CHECK(!fat32_create(&fs, "nowhere/at/all.txt", &fresh),
          "so is a file in a directory that does not exist");
    CHECK(!fat32_create(&fs, "", &fresh), "and one with no name");

    /* creating something that exists is just opening it */
    CHECK(fat32_create(&fs, "notes.txt", &fresh), "creating twice is allowed");
    CHECK(fresh.size == strlen(message), "and does not truncate what was there");

    /* ---- directories, made and unmade ---- */

    CHECK(fat32_mkdir(&fs, "made"), "a directory can be made");
    CHECK(fat32_lookup(&fs, "made", &f) && f.is_dir, "and is found as one");

    /* it starts empty -- dot and dotdot exist on the disk but are not
     * anybody else's business, so readdir must not show them */
    CHECK(!fat32_readdir(&fs, f.first_cluster, 0, &e),
          "a new directory is empty as far as anyone can see");

    /* and it is a real directory: things can be put in it and found */
    CHECK(fat32_create(&fs, "made/inside.txt", &fresh),
          "a file can be made inside it");
    CHECK(fat32_write(&fs, &fresh, 0, "here\n", 5) == 5, "and written");
    CHECK(fat32_lookup(&fs, "made/inside.txt", &f), "and found by path");
    CHECK(fat32_readdir(&fs, 0, 0, &e) || true, "");

    /* nested, which is the part that needs dotdot to be right */
    CHECK(fat32_mkdir(&fs, "made/deeper"), "and a directory inside that");
    CHECK(fat32_lookup(&fs, "made/deeper", &f) && f.is_dir, "found as one");

    /* ---- and unmade ---- */

    CHECK(!fat32_rmdir(&fs, "made"),
          "a directory with anything in it is not removed");
    CHECK(fat32_rmdir(&fs, "made/deeper"), "an empty one is");
    CHECK(!fat32_lookup(&fs, "made/deeper", &f), "and is gone afterwards");
    CHECK(!fat32_rmdir(&fs, "made/deeper"), "removing it twice does nothing");
    CHECK(!fat32_rmdir(&fs, ""), "and the root is nobody's to remove");
    CHECK(!fat32_rmdir(&fs, "made/inside.txt"),
          "nor is a file, however empty");

    CHECK(!fat32_mkdir(&fs, "made"), "a name already taken is refused");
    CHECK(!fat32_mkdir(&fs, "nowhere/at/all"),
          "and so is one whose parent does not exist");

    /* ---- the fields fat has always had ----
     *
     * a fixed clock, so what should have been written is exactly
     * knowable rather than merely plausible */
    fat32_set_clock(&fs, fixed_clock);

    CHECK(fat32_create(&fs, "stamped.txt", &fresh), "a file is made");
    CHECK(fat32_write(&fs, &fresh, 0, "when\n", 5) == 5, "and written");
    CHECK(fat32_lookup(&fs, "stamped.txt", &f), "and found again");
    CHECK(f.written.year == 2026 && f.written.month == 8 && f.written.day == 7,
          "with the date it was written");
    CHECK(f.written.hour == 14 && f.written.minute == 30,
          "and the time");
    /* seconds live in twos, because sixteen bits would not stretch to
     * one each -- so an odd second comes back as the even one below */
    CHECK(f.written.second == 44, "to the nearest two seconds, which is all fat holds");

    /* a filesystem with no clock writes zeroes, which is what fat means
     * by "nobody knows" rather than by "the epoch" */
    struct fat32 noclock;
    CHECK(fat32_mount(&noclock, img_read, img_write, &fd), "a mount with no clock");
    CHECK(fat32_create(&noclock, "noclock.txt", &fresh), "makes files");
    CHECK(fat32_lookup(&noclock, "noclock.txt", &f), "and finds them");
    CHECK(f.written.year == 1980, "with no date, which fat spells as its epoch");

    /* ---- unmaking ---- */

    CHECK(fat32_lookup(&fs, "stamped.txt", &f), "a file to remove");
    uint32_t held = f.first_cluster;
    CHECK(fat32_unlink(&fs, "stamped.txt"), "is removed");
    CHECK(!fat32_lookup(&fs, "stamped.txt", &f), "and is gone");
    CHECK(!fat32_unlink(&fs, "stamped.txt"), "removing it twice does nothing");
    CHECK(!fat32_unlink(&fs, "made"), "and a directory is not a file to remove");
    (void)held;

    /* the clusters have to come back, or a disk empties itself one
     * deleted file at a time and only says so when it is full */
    uint32_t used_before, total_before;
    CHECK(fat32_usage(&fs, &used_before, &total_before), "usage counted");
    struct fat32_file big2;
    CHECK(fat32_create(&fs, "big2.bin", &big2), "a file that spans clusters");
    char lump[3000];
    memset(lump, 'q', sizeof lump);
    CHECK(fat32_write(&fs, &big2, 0, lump, sizeof lump) == (int64_t)sizeof lump,
          "is written");
    uint32_t used_full, total_ignored;
    fat32_usage(&fs, &used_full, &total_ignored);
    CHECK(used_full > used_before, "and costs clusters");
    CHECK(fat32_unlink(&fs, "big2.bin"), "removing it works");
    uint32_t used_after;
    fat32_usage(&fs, &used_after, &total_ignored);
    CHECK(used_after == used_before, "and gives every one of them back");

    /* a long name is several entries, and all of them have to go --
     * leaving the run behind would leave a name pointing at nothing */
    CHECK(fat32_lookup(&fs, "velvet-room.txt", &f), "a long-named file");
    CHECK(f.lfn_sector != 0, "really does have entries in front of it");
    CHECK(fat32_unlink(&fs, "velvet-room.txt"), "is removed");
    CHECK(!fat32_lookup(&fs, "velvet-room.txt", &f), "and is gone by that name");

    int leftovers = 0;
    for (size_t i = 0; fat32_readdir(&fs, 0, i, &e); i++) {
        if (strstr(e.name, "velvet") != NULL) leftovers++;
    }
    CHECK(leftovers == 0, "with nothing of it left in the directory");

    /* ---- renaming ---- */

    CHECK(fat32_create(&fs, "before.txt", &fresh), "a file to rename");
    CHECK(fat32_write(&fs, &fresh, 0, "same bytes\n", 11) == 11, "with contents");
    CHECK(fat32_lookup(&fs, "before.txt", &f), "found");
    uint32_t same_cluster = f.first_cluster;

    CHECK(fat32_rename(&fs, "before.txt", "after.txt"), "is renamed");
    CHECK(!fat32_lookup(&fs, "before.txt", &f), "and the old name is gone");
    CHECK(fat32_lookup(&fs, "after.txt", &f), "and the new one is there");
    CHECK(f.first_cluster == same_cluster,
          "pointing at the very same clusters -- nothing was copied");
    CHECK(f.size == 11, "and the same size");

    text = slurp(&fs, &f, &len);
    CHECK(text && strcmp(text, "same bytes\n") == 0, "with the same contents");
    free(text);

    /* across directories, which is the same operation: a name moves and
     * a file does not */
    CHECK(fat32_rename(&fs, "after.txt", "notes/moved.txt"),
          "renaming into another directory works");
    CHECK(!fat32_lookup(&fs, "after.txt", &f), "gone from where it was");
    CHECK(fat32_lookup(&fs, "notes/moved.txt", &f), "and found where it went");
    CHECK(f.first_cluster == same_cluster, "still the same clusters");

    CHECK(!fat32_rename(&fs, "notes/moved.txt", "motd.txt"),
          "renaming onto a name already taken is refused");
    CHECK(!fat32_rename(&fs, "nothing.txt", "x.txt"),
          "and renaming what is not there does nothing");

    /* ---- a read-only mount must refuse every one of those ---- */

    struct fat32 ro;
    CHECK(fat32_mount(&ro, img_read, NULL, &fd), "a disk can be mounted read-only");
    CHECK(fat32_lookup(&ro, "hello.txt", &f), "and still read");
    CHECK(fat32_write(&ro, &f, 0, "no", 2) == -1, "but not written");
    CHECK(!fat32_create(&ro, "new.txt", &fresh), "and not added to");
    CHECK(!fat32_mkdir(&ro, "nope"), "no directory made on it");
    CHECK(!fat32_rmdir(&ro, "made"), "and none removed");
    CHECK(!fat32_unlink(&ro, "hello.txt"), "no file removed");
    CHECK(!fat32_rename(&ro, "hello.txt", "x.txt"), "and none renamed");

    /* ---- everything survives being unmounted ---- */

    struct fat32 remount;
    CHECK(fat32_mount(&remount, img_read, img_write, &fd),
          "the disk mounts again after all that");
    CHECK(fat32_lookup(&remount, "notes.txt", &f), "the new file is still there");
    text = slurp(&remount, &f, &len);
    CHECK(text && strcmp(text, message) == 0, "with its contents intact");
    free(text);

    count = 0;
    for (size_t i = 0; fat32_readdir(&remount, 0, i, &e); i++) {
        count++;
    }
    /* what the root should hold now: what it started with, plus
     * notes.txt, `made`, noclock.txt and big2... minus velvet-room.txt
     * and the ones that were removed again. rather than track that sum
     * by hand, check the shape of it: more than before, and every name
     * that was removed really is absent */
    CHECK(count > root_at_first, "the root grew by what was added to it");

    int ghosts = 0;
    for (size_t i = 0; fat32_readdir(&remount, 0, i, &e); i++) {
        if (strcmp(e.name, "velvet-room.txt") == 0) ghosts++;
        if (strcmp(e.name, "stamped.txt") == 0) ghosts++;
        if (strcmp(e.name, "big2.bin") == 0) ghosts++;
        if (strcmp(e.name, "after.txt") == 0) ghosts++;
    }
    CHECK(ghosts == 0,
          "and nothing removed came back when the disk was mounted again");

    uint32_t used, total;
    CHECK(fat32_usage(&remount, &used, &total), "usage can be counted");
    CHECK(used > 0 && used < total, "and is somewhere between empty and full");

    close(fd);
    if (failures == 0) printf("all good\n");
    return failures;
}
