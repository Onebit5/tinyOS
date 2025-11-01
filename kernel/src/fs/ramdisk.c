#include "fs/ramdisk.h"
#include "lib/string.h"

#define TAR_BLOCK 512

/* the ustar header. every numeric field is ascii octal, because in 1979
 * that was the portable choice and nobody has been brave enough to
 * change it since */
struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];      /* "ustar" */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

_Static_assert(sizeof(struct tar_header) == TAR_BLOCK,
               "a tar header is one block, no more and no less");

static const uint8_t *archive;
static uint64_t archive_size;
static size_t file_count;

/* ascii octal, possibly padded with spaces or terminated early. tar
 * writers disagree about the details, so be forgiving */
static uint64_t parse_octal(const char *field, size_t len) {
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = field[i];
        if (c == '\0' || c == ' ') {
            if (v != 0) {
                break;      /* trailing padding after the digits */
            }
            continue;       /* leading padding before them */
        }
        if (c < '0' || c > '7') {
            break;
        }
        v = v * 8 + (uint64_t)(c - '0');
    }
    return v;
}

static bool is_ustar(const struct tar_header *h) {
    return h->magic[0] == 'u' && h->magic[1] == 's' && h->magic[2] == 't'
        && h->magic[3] == 'a' && h->magic[4] == 'r';
}

/* the header at a given byte offset, or NULL if we have run off the end
 * or hit the zero blocks that mark the finish */
static const struct tar_header *header_at(uint64_t offset) {
    if (archive == NULL || offset + TAR_BLOCK > archive_size) {
        return NULL;
    }
    const struct tar_header *h = (const struct tar_header *)(archive + offset);
    if (h->name[0] == '\0') {
        return NULL;        /* end of archive */
    }
    if (!is_ustar(h)) {
        return NULL;        /* not something we understand, stop rather
                             * than wander off into the bytes */
    }
    return h;
}

/* skip past a header and its contents to the next header */
static uint64_t next_offset(uint64_t offset, const struct tar_header *h) {
    uint64_t size = parse_octal(h->size, sizeof h->size);
    uint64_t blocks = (size + TAR_BLOCK - 1) / TAR_BLOCK;
    return offset + TAR_BLOCK + blocks * TAR_BLOCK;
}

void ramdisk_mount(const void *base, uint64_t size) {
    archive = base;
    archive_size = size;
    file_count = 0;

    for (uint64_t off = 0; ; ) {
        const struct tar_header *h = header_at(off);
        if (h == NULL) {
            break;
        }
        /* typeflag '0' and '\0' both mean a normal file. directories
         * ('5') exist in the archive but we have nothing to do with
         * them, so they are counted and then ignored on lookup */
        file_count++;
        off = next_offset(off, h);
    }
}

bool ramdisk_stat(size_t index, struct ramdisk_file *out) {
    size_t i = 0;
    for (uint64_t off = 0; ; i++) {
        const struct tar_header *h = header_at(off);
        if (h == NULL) {
            return false;
        }
        if (i == index) {
            out->name = h->name;
            out->size = parse_octal(h->size, sizeof h->size);
            out->data = archive + off + TAR_BLOCK;
            out->mode = (uint32_t)parse_octal(h->mode, sizeof h->mode);
            return true;
        }
        off = next_offset(off, h);
    }
}

bool ramdisk_open(const char *name, struct ramdisk_file *out) {
    /* tar keeps "./foo" and "foo" as different names depending on how
     * it was made, so let a leading ./ be optional on both sides */
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
    }

    struct ramdisk_file f;
    for (size_t i = 0; ramdisk_stat(i, &f); i++) {
        const char *have = f.name;
        if (have[0] == '.' && have[1] == '/') {
            have += 2;
        }
        if (strcmp(have, name) == 0) {
            *out = f;
            return true;
        }
    }
    return false;
}

bool ramdisk_may_read(const struct ramdisk_file *f, int uid) {
    if (uid == 0) {
        return true;        /* the master of the velvet room reads all */
    }
    return (f->mode & 0004) != 0;   /* everyone else needs other-read */
}

bool     ramdisk_present(void) { return archive != NULL && file_count > 0; }
uint64_t ramdisk_bytes(void)   { return archive_size; }
size_t   ramdisk_count(void)   { return file_count; }

#ifndef TINYOS_HOSTED

#include "limine.h"
#include "lib/kprintf.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
};

void ramdisk_init(void) {
    if (module_request.response == NULL
        || module_request.response->module_count == 0) {
        kprintf("ramdisk    : none supplied, the shelves are bare\n");
        return;
    }

    /* the module's *bytes* are safe -- limine puts them in memory typed
     * "kernel and modules", which we never reclaim. this response
     * struct is not, so copy what we need and never look again */
    struct limine_file *m = module_request.response->modules[0];
    ramdisk_mount(m->address, m->size);

    kprintf("ramdisk    : %lu KiB, %zu files\n",
            ramdisk_bytes() / 1024, ramdisk_count());
}

#endif
