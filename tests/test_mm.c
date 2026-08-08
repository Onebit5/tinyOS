/* host-side test for pmm + kmalloc. fabricates a limine memory map whose
 * "physical" addresses are offsets into a big aligned host buffer, with
 * hhdm = buffer base, so phys_to_virt lands back inside the buffer */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* kernel stubs */
void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void kvprintf(const char *fmt, va_list ap) { vprintf(fmt, ap); }
void panic(const char *fmt, ...) {
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "mm/pmm.h"
#include "mm/kmalloc.h"

#define ARENA (8ull * 1024 * 1024)
#define MiB   (1024ull * 1024)

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    void *arena = aligned_alloc(4096, ARENA);
    uint64_t hhdm = (uint64_t)arena;

    /* usable 4k..1MiB, a reserved hole, usable 2MiB..8MiB */
    struct limine_memmap_entry e0 = { .base = 0x1000, .length = MiB - 0x1000,
                                      .type = LIMINE_MEMMAP_USABLE };
    struct limine_memmap_entry e1 = { .base = MiB, .length = MiB,
                                      .type = LIMINE_MEMMAP_RESERVED };
    struct limine_memmap_entry e2 = { .base = 2 * MiB, .length = 6 * MiB,
                                      .type = LIMINE_MEMMAP_USABLE };
    struct limine_memmap_entry *map[3] = { &e0, &e1, &e2 };

    pmm_init_from_map(map, 3, hhdm);

    uint64_t usable = (MiB - 0x1000) + 6 * MiB;
    CHECK(pmm_total_bytes() == usable, "total == sum of usable regions");
    /* bitmap for 8MiB of frames fits in one page, parked in e0 */
    CHECK(pmm_free_bytes() == usable - PAGE_SIZE, "free == total - bitmap page");

    /* single frame round trip */
    uint64_t f = pmm_alloc();
    CHECK(f != 0 && f % PAGE_SIZE == 0, "alloc gives an aligned frame");
    CHECK((f >= 0x1000 && f < MiB) || (f >= 2 * MiB && f < 8 * MiB),
          "frame is inside a usable region");
    memset(pmm_phys_to_virt(f), 0xab, PAGE_SIZE);
    pmm_free(f);
    CHECK(pmm_free_bytes() == usable - PAGE_SIZE, "books balance after 1 frame");

    /* contiguous run: 4 frames, writable end to end */
    uint64_t run = pmm_alloc_pages(4);
    CHECK(run != 0, "got a 4-frame run");
    memset(pmm_phys_to_virt(run), 0xcd, 4 * PAGE_SIZE);
    pmm_free_pages(run, 4);

    /* drain it dry: every frame handed out exactly once, no overlaps */
    uint64_t expect_frames = pmm_free_bytes() / PAGE_SIZE;
    static uint8_t seen[ARENA / PAGE_SIZE];
    memset(seen, 0, sizeof(seen));
    uint64_t got = 0;
    uint64_t addr;
    while ((addr = pmm_alloc()) != 0) {
        uint64_t idx = addr / PAGE_SIZE;
        CHECK(idx < ARENA / PAGE_SIZE, "frame within arena");
        CHECK(!seen[idx], "frame never handed out twice");
        seen[idx] = 1;
        got++;
    }
    CHECK(got == expect_frames, "drained exactly the advertised number");
    CHECK(pmm_free_bytes() == 0, "free == 0 when dry");
    CHECK(pmm_alloc_pages(1) == 0, "alloc when dry says 0");
    /* give everything back */
    for (uint64_t i = 0; i < ARENA / PAGE_SIZE; i++) {
        if (seen[i]) pmm_free(i * PAGE_SIZE);
    }
    CHECK(pmm_free_bytes() == expect_frames * PAGE_SIZE, "all returned");

    /* ---- heap on top ---- */
    uint64_t used0 = kheap_used_bytes();
    size_t sizes[6] = { 1, 24, 1000, 16384, 512, 4096 };
    uint8_t *p[6];
    for (int i = 0; i < 6; i++) {
        p[i] = kmalloc(sizes[i]);
        CHECK(p[i] != NULL, "kmalloc says yes");
        CHECK(((uint64_t)p[i] & 15) == 0, "payload 16-byte aligned");
        memset(p[i], 0x40 + i, sizes[i]);
    }
    for (int i = 0; i < 6; i++) {
        int ok = 1;
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[i][j] != (uint8_t)(0x40 + i)) ok = 0;
        }
        CHECK(ok, "heap block kept its pattern (no overlap)");
    }
    int order[6] = { 3, 0, 5, 1, 4, 2 };
    for (int i = 0; i < 6; i++) kfree(p[order[i]]);
    CHECK(kheap_used_bytes() == used0, "heap books balance");

    /* coalescing: three smalls freed then one bigger alloc, heap total
     * must not grow (the merged block gets reused) */
    uint64_t total0 = kheap_total_bytes();
    uint8_t *a = kmalloc(100), *b = kmalloc(100), *c = kmalloc(100);
    kfree(a); kfree(b); kfree(c);
    uint8_t *big = kmalloc(300);
    CHECK(kheap_total_bytes() == total0, "coalesced space reused, no growth");
    kfree(big);

    /* zero and absurd */
    CHECK(kmalloc(0) == NULL, "kmalloc(0) is NULL");
    CHECK(kmalloc(64 * MiB) == NULL, "kmalloc beyond ram is NULL, not a crash");
    uint8_t *after = kmalloc(64);
    CHECK(after != NULL, "heap still works after an oom");
    kfree(after);

    if (failures == 0) printf("all good\n");
    return failures;
}
