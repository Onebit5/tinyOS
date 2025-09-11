/* the elf header checks. elf_load itself maps pages and needs a real
 * cpu, but everything it refuses to load is decided here, and a loader
 * that maps nonsense is a loader that hands ring 3 the kernel. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fs/elf.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/* a minimal but valid elf64 header, which each test then breaks in one
 * specific way */
static uint8_t img[4096];

static void reset(void) {
    memset(img, 0, sizeof img);
    img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = 2;                     /* 64-bit */
    img[5] = 1;                     /* little endian */
    *(uint16_t *)(img + 16) = 2;    /* ET_EXEC */
    *(uint16_t *)(img + 18) = 0x3e; /* x86-64 */
    *(uint64_t *)(img + 24) = 0x400000;  /* entry */
    *(uint64_t *)(img + 32) = 64;        /* phoff */
    *(uint16_t *)(img + 54) = 56;        /* phentsize */
    *(uint16_t *)(img + 56) = 1;         /* phnum */
}

static void refuses(const char *what) {
    const char *why = NULL;
    if (elf_is_loadable(img, sizeof img, &why)) {
        printf("FAIL: accepted an image that %s\n", what);
        failures++;
    }
}

int main(void) {
    const char *why = NULL;

    reset();
    CHECK(elf_is_loadable(img, sizeof img, &why),
          "a well formed header is accepted");

    CHECK(!elf_is_loadable(NULL, 0, &why), "NULL is not an elf");
    CHECK(!elf_is_loadable(img, 8, &why), "eight bytes is not an elf");
    CHECK(elf_is_loadable(img, sizeof img, NULL),
          "the reason pointer may be NULL");

    reset(); img[1] = 'X';                      refuses("has no elf magic");
    reset(); img[4] = 1;                        refuses("is 32-bit");
    reset(); img[5] = 2;                        refuses("is big endian");
    reset(); *(uint16_t *)(img + 18) = 0x28;    refuses("is arm");
    reset(); *(uint16_t *)(img + 16) = 3;       refuses("is a shared object");
    reset(); *(uint16_t *)(img + 16) = 1;       refuses("is a relocatable object");
    reset(); *(uint16_t *)(img + 56) = 0;       refuses("has no program headers");
    reset(); *(uint16_t *)(img + 54) = 32;      refuses("has odd-sized phdrs");
    reset(); *(uint64_t *)(img + 24) = 0;       refuses("has no entry point");

    /* the one that matters most: headers pointing past the file. a
     * loader that trusts this reads whatever follows in memory */
    reset(); *(uint64_t *)(img + 32) = 8192;
    refuses("puts its program headers past the end of the file");

    reset();
    *(uint64_t *)(img + 32) = 64;
    *(uint16_t *)(img + 56) = 1000;   /* 1000 * 56 bytes runs off the end */
    refuses("claims more program headers than fit");

    /* and the real program the build makes */
    {
        FILE *fp = fopen("ramdisk/bin/hello", "rb");
        if (fp == NULL) {
            printf("  (skipping the real binary: run `make ramdisk/bin/hello`)\n");
        } else {
            static uint8_t real[256 * 1024];
            size_t n = fread(real, 1, sizeof real, fp);
            fclose(fp);
            why = NULL;
            CHECK(elf_is_loadable(real, n, &why),
                  "the userspace program the build produces is loadable");
            if (why) printf("    (it said: %s)\n", why);
        }
    }

    if (!failures) printf("all good\n");
    return failures;
}
