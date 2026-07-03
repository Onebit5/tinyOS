/* host-side test for the one namespace.
 *
 * the disk is stubbed rather than real -- test_fat32 runs the actual
 * filesystem against an actual image -- because what this file is
 * responsible for is the *resolution*: which mount a name lands on,
 * what happens when two of them have the same name, and above all what
 * happens when there is no disk at all.
 *
 * that last one is the reason the ramdisk still exists. every check
 * below is run twice, once with a disk and once without, and the
 * without-a-disk half has to keep working. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

void panic(const char *fmt, ...) {
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

#include "fs/vfs.h"
#include "fs/disk.h"
#include "fs/ramdisk.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* ---- a disk I can switch off ---- */

static bool have_disk = true;

static const char disk_welcome[] = "the disk's copy\n";
static const char disk_notes[]   = "notes on the disk\n";

/* a filesystem that has opinions about its own files. before 0.2.14
 * the vfs decreed 0644 owned by root for everything on the disk,
 * because fat has nowhere to record anything else -- now it asks, and
 * these are the answers */
static const struct {
    const char *path, *dir, *name, *body;
    bool is_dir;
    unsigned mode, uid;
} disk_tree[] = {
    { "/welcome.txt", "/", "welcome.txt", disk_welcome, false, 0644, 0 },
    { "/notes.txt",   "/", "notes.txt",   disk_notes,   false, 0644, 0 },
    { "/deep",        "/", "deep",        NULL,         true,  0755, 0 },
    { "/deep/x.txt",  "/deep", "x.txt",   "nested\n",   false, 0600, 1000 },
};
#define DISK_COUNT (sizeof disk_tree / sizeof disk_tree[0])

bool disk_ready(void) { return have_disk; }

/* fat32 strips every leading slash and skips empty components, so "//"
 * and "/" are the same directory to it. the stub has to agree, or it
 * would be testing itself rather than the thing above it */
static const char *tidy(const char *path) {
    while (path[0] == '/' && path[1] == '/') path++;
    return path;
}

bool disk_lookup(const char *path, struct disk_entry *out) {
    path = tidy(path);
    if (!have_disk) return false;
    for (size_t i = 0; i < DISK_COUNT; i++) {
        if (strcmp(disk_tree[i].path, path) != 0) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        out->size = disk_tree[i].body ? strlen(disk_tree[i].body) : 0;
        out->cluster = (uint32_t)(i + 2);
        out->entry_sector = 100 + i;
        out->mode = disk_tree[i].mode;
        out->uid = disk_tree[i].uid;
        return true;
    }
    return false;
}

bool disk_readdir(const char *path, size_t index, struct disk_entry *out) {
    path = tidy(path);
    if (!have_disk) return false;
    size_t seen = 0;
    for (size_t i = 0; i < DISK_COUNT; i++) {
        if (strcmp(disk_tree[i].dir, path) != 0) continue;
        if (seen++ != index) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        out->size = disk_tree[i].body ? strlen(disk_tree[i].body) : 0;
        out->cluster = (uint32_t)(i + 2);
        out->mode = disk_tree[i].mode;
        out->uid = disk_tree[i].uid;
        return true;
    }
    return false;
}

int64_t disk_read(uint32_t cluster, uint64_t size, uint64_t offset,
                  void *buf, uint64_t len) {
    (void)size;
    size_t i = cluster - 2;
    if (!have_disk || i >= DISK_COUNT || disk_tree[i].body == NULL) return -1;
    uint64_t total = strlen(disk_tree[i].body);
    if (offset >= total) return 0;
    if (offset + len > total) len = total - offset;
    memcpy(buf, disk_tree[i].body + offset, len);
    return (int64_t)len;
}

static int creates;
bool disk_create(const char *path, struct disk_entry *out) {
    if (!have_disk) return false;
    creates++;
    memset(out, 0, sizeof *out);
    strcpy(out->name, "made.txt");
    out->entry_sector = 999;
    (void)path;
    return true;
}
int64_t disk_write_at(struct disk_entry *e, uint64_t offset, const void *buf,
                      uint64_t len) {
    (void)buf;
    if (!have_disk) return -1;
    e->size = offset + len;
    return (int64_t)len;
}
const char *disk_model(void) { return "STUB DISK"; }
static int mkdirs, rmdirs, unlinks, renames;
bool disk_mkdir(const char *path) { (void)path; mkdirs++; return have_disk; }
bool disk_rmdir(const char *path) { (void)path; rmdirs++; return have_disk; }
bool disk_unlink(const char *path) { (void)path; unlinks++; return have_disk; }
/* the things a filesystem with opinions can be told. every one of them
 * answers false on a disk that has nowhere to record the answer, which
 * is what the stub is standing in for here */
bool disk_lookup_nofollow(const char *path, struct disk_entry *out) {
    return disk_lookup(path, out);
}
bool disk_readlink(const char *p, char *o, size_t n) {
    (void)p; (void)o; (void)n; return false;
}
bool disk_chmod(const char *p, uint32_t m) { (void)p; (void)m; return false; }
/* which filesystem answered. the stub disk is not either of the real
 * ones, and saying so is more honest than picking a name */
const char *disk_kind_name(void) { return "stub"; }
enum disk_kind disk_which(void) { return DISK_FAT32; }
bool disk_chown(const char *p, uint32_t u, uint32_t g) {
    (void)p; (void)u; (void)g; return false;
}
bool disk_symlink(const char *p, const char *t) {
    (void)p; (void)t; return false;
}

bool disk_rename(const char *from, const char *to) {
    (void)from; (void)to; renames++; return have_disk;
}

/* ---- a ramdisk, built as a real tar so the parser is the real one ---- */

static void octal(char *dst, uint64_t v, size_t width) {
    for (size_t i = width - 1; i > 0; i--) {
        dst[i - 1] = (char)('0' + (v & 7));
        v >>= 3;
    }
    dst[width - 1] = '\0';
}

static size_t add_file(uint8_t *tar, size_t at, const char *name,
                       const char *body, unsigned mode) {
    uint8_t *h = tar + at;
    memset(h, 0, 512);
    strcpy((char *)h, name);
    octal((char *)h + 100, mode, 8);
    octal((char *)h + 124, strlen(body), 12);
    memcpy(h + 257, "ustar", 5);
    h[156] = '0';

    unsigned sum = 0;
    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++) sum += h[i];
    octal((char *)h + 148, sum, 8);

    at += 512;
    memcpy(tar + at, body, strlen(body));
    at += (strlen(body) + 511) / 512 * 512;
    return at;
}

int main(void) {
    static uint8_t tar[64 * 1024];
    size_t at = 0;
    at = add_file(tar, at, "./welcome.txt", "the ramdisk's copy\n", 0644);
    at = add_file(tar, at, "./passwd", "igor:velvet:0\n", 0600);
    at = add_file(tar, at, "./bin/hello", "ELF-ish\n", 0755);
    at = add_file(tar, at, "./secret.txt", "no peeking\n", 0600);
    ramdisk_mount(tar, sizeof tar);

    CHECK(ramdisk_present(), "the ramdisk mounted");

    struct vfs_file f;
    char buf[128];

    /* ---- with a disk ---- */
    have_disk = true;

    CHECK(vfs_open("/welcome.txt", &f) && f.kind == VFS_DISK,
          "an absolute name is the disk's");
    CHECK(vfs_read(&f, 0, buf, f.size) == (int64_t)f.size, "and reads");
    buf[f.size] = '\0';
    CHECK(strcmp(buf, "the disk's copy\n") == 0, "with the disk's contents");

    CHECK(vfs_open("/boot/welcome.txt", &f) && f.kind == VFS_RAMDISK,
          "a name under /boot is the ramdisk's");
    CHECK(vfs_read(&f, 0, buf, f.size) == (int64_t)f.size, "and reads");
    buf[f.size] = '\0';
    CHECK(strcmp(buf, "the ramdisk's copy\n") == 0,
          "with the ramdisk's contents, which are different");

    /* the search order, which is the whole point of the arrangement */
    CHECK(vfs_open("welcome.txt", &f) && f.kind == VFS_DISK,
          "a bare name finds the disk's copy first");
    CHECK(vfs_open("bin/hello", &f) && f.kind == VFS_RAMDISK,
          "and falls through to the ramdisk when the disk has none");
    CHECK(vfs_open("passwd", &f) && f.kind == VFS_RAMDISK,
          "which is how passwd is still found");

    CHECK(!vfs_open("nothing-at-all", &f), "a name on neither is on neither");
    CHECK(!vfs_open("/bootleg/x", &f), "and /bootleg is not /boot");
    CHECK(!vfs_open("", &f), "an empty path is nothing");

    /* subdirectories on the disk */
    CHECK(vfs_open("/deep/x.txt", &f) && f.kind == VFS_DISK,
          "a nested disk path resolves");

    /* ---- the mount points themselves ----
     *
     * neither of these is on any filesystem: `/` is where the mounts
     * hang from and `/boot` *is* the ramdisk. something still has to be
     * able to say they are directories, or nobody can stand in one */
    CHECK(vfs_open("/", &f) && f.is_dir, "the root is a directory");
    CHECK(vfs_open("/boot", &f) && f.is_dir, "and so is the mount point");
    CHECK(vfs_open("/boot/", &f) && f.is_dir, "however it is spelled");

    /* ---- listing ---- */

    int saw_boot = 0, saw_welcome = 0, count = 0;
    for (size_t i = 0; vfs_readdir("/", i, &f); i++) {
        if (strcmp(f.name, "boot") == 0 && f.is_dir) saw_boot = 1;
        if (strcmp(f.name, "welcome.txt") == 0) saw_welcome = 1;
        count++;
    }
    CHECK(saw_welcome, "the root lists what is on the disk");
    CHECK(saw_boot, "and the mount standing on it, which is on no filesystem");
    CHECK(count == (int)DISK_COUNT - 1 + 1,
          "exactly the disk's root plus that one mount");

    int saw_hello = 0, saw_dirs = 0;
    count = 0;
    for (size_t i = 0; vfs_readdir("/boot", i, &f); i++) {
        if (strcmp(f.name, "bin/hello") == 0) saw_hello = 1;
        if (f.is_dir) saw_dirs++;
        count++;
    }
    CHECK(saw_hello, "/boot lists the ramdisk");
    CHECK(count == 4, "all of it");
    CHECK(saw_dirs == 0, "and none of tar's directory records");

    /* ---- relative directories, which `ls boot/` needs ----
     * a relative filename always worked; a relative directory did not,
     * and there is no reason for anyone to expect that difference */

    count = 0;
    for (size_t i = 0; vfs_readdir("boot", i, &f); i++) count++;
    CHECK(count == 4, "a relative directory lists what the absolute one does");

    count = 0;
    for (size_t i = 0; vfs_readdir("boot/", i, &f); i++) count++;
    CHECK(count == 4, "and a trailing slash changes nothing");

    count = 0;
    for (size_t i = 0; vfs_readdir("/boot/", i, &f); i++) count++;
    CHECK(count == 4, "nor does one on an absolute path");

    count = 0;
    for (size_t i = 0; vfs_readdir("deep", i, &f); i++) count++;
    CHECK(count == 1, "a relative directory on the disk works too");

    count = 0;
    for (size_t i = 0; vfs_readdir("//", i, &f); i++) count++;
    CHECK(count == (int)DISK_COUNT - 1 + 1, "and a doubled slash is the root");

    CHECK(!vfs_readdir("bootleg", 0, &f), "but bootleg is still not boot");

    /* ---- a directory inside the ramdisk ----
     *
     * the archive is flat: it holds a name like "bin/hello", not a
     * directory called bin with a hello in it. but /boot/bin/hello can
     * be opened, so /boot/bin has to be listable too, or the namespace
     * says two different things depending on which question is asked */
    count = 0;
    int saw_bare_hello = 0;
    for (size_t i = 0; vfs_readdir("/boot/bin", i, &f); i++) {
        if (strcmp(f.name, "hello") == 0) saw_bare_hello = 1;
        count++;
    }
    CHECK(count == 1, "a directory inside the ramdisk lists what is in it");
    CHECK(saw_bare_hello,
          "under the name it has there, not the one the archive stores");
    CHECK(vfs_open("/boot/bin/hello", &f),
          "and the same name opens, which is the point");

    CHECK(!vfs_readdir("/boot/nothing", 0, &f),
          "a directory that is in no name lists nothing");

    /* ---- permissions still come off the file ---- */

    CHECK(vfs_open("/boot/secret.txt", &f), "a private file is found");
    CHECK(vfs_may_read(&f, 0), "the master may read it");
    CHECK(!vfs_may_read(&f, 1000), "and nobody else may");
    CHECK(vfs_open("/boot/welcome.txt", &f) && vfs_may_read(&f, 1000),
          "a 0644 file is readable by anyone");

    /* until 0.2.14 every file on the disk was 0644 owned by root by
     * decree, because fat has nowhere to record anything else. now the
     * filesystem answers for itself, and two files on one disk can
     * disagree -- which is the entire point of that version */
    CHECK(vfs_open("/welcome.txt", &f) && vfs_may_read(&f, 1000),
          "a 0644 file on the disk is readable by anyone");
    CHECK(vfs_open("/welcome.txt", &f) && f.mode == 0644,
          "with the mode the filesystem gave it rather than one I decreed");

    CHECK(vfs_open("/deep/x.txt", &f), "and a 0600 one is found");
    CHECK(f.mode == 0600 && f.uid == 1000,
          "with its own mode and its own owner");
    CHECK(!vfs_may_read(&f, 1000) ? false : true,
          "readable by whoever owns it");
    CHECK(!vfs_may_read(&f, 2000),
          "and not by somebody else -- a sentence the disk could not say "
          "at all before there was a filesystem to say it in");
    CHECK(vfs_may_read(&f, 0), "though the master may read anything");

    /* ---- writing ---- */

    CHECK(vfs_open("/notes.txt", &f) && vfs_writable(&f), "the disk is writable");
    CHECK(vfs_write(&f, 0, "x", 1) == 1, "and takes a write");
    CHECK(vfs_open("/boot/welcome.txt", &f) && !vfs_writable(&f),
          "the ramdisk is not");
    CHECK(vfs_write(&f, 0, "x", 1) == -1, "and refuses one");
    CHECK(!vfs_create("/boot/new.txt", &f),
          "nothing can be made under /boot -- it is read-only memory");
    CHECK(!vfs_mkdir("/boot/somedir"), "nor a directory under it");
    CHECK(!vfs_rmdir("/boot"), "and it is not removable either");
    CHECK(vfs_mkdir("/somedir"), "but the disk takes one");
    CHECK(vfs_rmdir("/somedir"), "and gives it back");
    CHECK(!vfs_unlink("/boot/welcome.txt"), "nor a file removed from it");
    CHECK(vfs_create("/new.txt", &f), "but the disk will make a file");

    /* ---- unmaking, and moving ---- */

    unlinks = renames = 0;
    CHECK(!vfs_unlink("/boot/welcome.txt"),
          "nothing under /boot can be removed either");
    CHECK(unlinks == 0, "and the disk is not even asked about it");
    CHECK(vfs_unlink("/notes.txt"), "but a file on the disk goes");
    CHECK(unlinks == 1, "by asking the disk exactly once");

    CHECK(vfs_rename("/a.txt", "/b.txt"), "a rename on the disk is passed on");
    CHECK(renames == 1, "once");

    /* a rename with either end on the ramdisk would be a copy pretending
     * to be a rename -- and one of those quietly costs a whole file's
     * worth of reading and writing where a rename costs none */
    renames = 0;
    CHECK(!vfs_rename("/boot/welcome.txt", "/moved.txt"),
          "a rename out of /boot is refused");
    CHECK(!vfs_rename("/notes.txt", "/boot/moved.txt"),
          "and so is one into it");
    CHECK(renames == 0, "with the disk never asked to attempt either");

    CHECK(!vfs_unlink(""), "an empty path removes nothing");
    CHECK(!vfs_rename("/a.txt", ""), "and renames nothing");

    /* ---- whole files ---- */

    const void *data;
    uint64_t size;
    bool owned;

    CHECK(vfs_slurp("bin/hello", &data, &size, &owned), "a program slurps");
    CHECK(!owned, "off the ramdisk without being copied at all");
    vfs_release(data, owned);

    CHECK(vfs_slurp("/notes.txt", &data, &size, &owned), "and one off the disk");
    CHECK(owned, "which had to be read into memory first");
    CHECK(size == strlen(disk_notes) && memcmp(data, disk_notes, size) == 0,
          "with the right bytes");
    vfs_release(data, owned);

    /* ---- and now the part that matters: no disk at all ---- */
    have_disk = false;

    CHECK(vfs_open("bin/hello", &f) && f.kind == VFS_RAMDISK,
          "every program is still found with no disk");
    CHECK(vfs_open("passwd", &f) && f.kind == VFS_RAMDISK,
          "and so is passwd, so the machine can still be logged into");
    CHECK(vfs_open("welcome.txt", &f) && f.kind == VFS_RAMDISK,
          "a bare name falls all the way through to the ramdisk");
    CHECK(vfs_open("/boot/welcome.txt", &f), "and /boot is where it always was");

    /* an absolute name still finds the ramdisk's copy. that is the
     * fallback doing its job rather than a leak: every path arrives here
     * absolute now, resolved against wherever the caller was standing,
     * so if `/x` did not fall through then a machine with no disk could
     * reach no program, no passwd and no file at all */
    CHECK(vfs_open("/welcome.txt", &f) && f.kind == VFS_RAMDISK,
          "an absolute name falls through to the ramdisk with no disk");
    CHECK(!vfs_open("/notes.txt", &f),
          "but a name on neither is still on neither");
    CHECK(!vfs_create("/anything.txt", &f), "and nothing can be made");
    CHECK(!vfs_unlink("/welcome.txt"),
          "nor unmade -- the copy that is left lives in read-only memory");
    CHECK(!vfs_rename("/welcome.txt", "/other.txt"), "nor renamed");

    count = 0;
    saw_boot = 0;
    for (size_t i = 0; vfs_readdir("/", i, &f); i++) {
        if (strcmp(f.name, "boot") == 0) saw_boot = 1;
        count++;
    }
    CHECK(saw_boot && count == 1,
          "the root holds nothing but /boot, which is honest");

    count = 0;
    for (size_t i = 0; vfs_readdir("/boot", i, &f); i++) {
        count++;
    }
    CHECK(count == 4, "and /boot still lists everything");

    CHECK(vfs_slurp("bin/hello", &data, &size, &owned) && !owned,
          "programs still load, which is the whole reason to keep it");
    vfs_release(data, owned);

    /* ---- the mount table ---- */

    struct vfs_mount m;
    CHECK(vfs_mount_at(0, &m) && strcmp(m.at, "/") == 0, "/ is a mount");
    CHECK(!m.present, "and says so when there is no disk behind it");
    CHECK(vfs_mount_at(1, &m) && strcmp(m.at, VFS_BOOT) == 0, "/boot is one");
    CHECK(m.present && !m.writable, "always there, never writable");
    CHECK(!vfs_mount_at(2, &m), "and there are only the two");

    if (failures == 0) printf("all good\n");
    return failures;
}
