#include "mm/pmm.h"
#include "lib/kprintf.h"
#include <stdbool.h>
#include "lib/panic.h"
#include "lib/string.h"

/* limine, tell us what ram looks like and where you mirrored it */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

static uint64_t hhdm_offset;
static uint64_t *bitmap;        /* 1 bit per frame, set = used */
static uint64_t bitmap_frames;  /* how many frames the bitmap tracks */
static uint64_t total_frames;   /* usable frames overall */
static uint64_t free_frames;
static uint64_t search_hint;    /* frame index to start scanning from */

static const char *memmap_type_name(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad memory (yikes)";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader (reclaimable)";
    case LIMINE_MEMMAP_KERNEL_AND_MODULES:     return "kernel + modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    default:                                   return "???";
    }
}

static inline void bit_set(uint64_t frame)   { bitmap[frame / 64] |=  (1ull << (frame % 64)); }
static inline void bit_clear(uint64_t frame) { bitmap[frame / 64] &= ~(1ull << (frame % 64)); }
static inline bool bit_test(uint64_t frame)  { return bitmap[frame / 64] & (1ull << (frame % 64)); }

void pmm_init_from_map(struct limine_memmap_entry **entries, size_t count,
                       uint64_t hhdm) {
    hhdm_offset = hhdm;

    /* pass 1: how far up does usable ram go, and how much is there */
    uint64_t highest = 0;
    total_frames = 0;
    for (size_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = entries[i];
        kprintf("  %016lx - %016lx  %s\n", e->base, e->base + e->length,
                memmap_type_name(e->type));
        if (e->type == LIMINE_MEMMAP_USABLE) {
            total_frames += e->length / PAGE_SIZE;
            if (e->base + e->length > highest) {
                highest = e->base + e->length;
            }
        }
    }

    bitmap_frames = highest / PAGE_SIZE;
    uint64_t bitmap_bytes = (bitmap_frames + 7) / 8;

    /* pass 2: find a usable region big enough to park the bitmap in */
    bitmap = NULL;
    for (size_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes) {
            bitmap = (uint64_t *)(e->base + hhdm_offset);
            break;
        }
    }
    if (bitmap == NULL) {
        panic("nowhere to put the pmm bitmap (%lu bytes). how little ram is this?",
              bitmap_bytes);
    }

    /* everything starts used, then usable regions get freed, then the
     * bitmap makes its own bed */
    memset(bitmap, 0xff, bitmap_bytes);
    free_frames = 0;
    for (size_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        for (uint64_t f = e->base / PAGE_SIZE;
             f < (e->base + e->length) / PAGE_SIZE; f++) {
            bit_clear(f);
            free_frames++;
        }
    }

    uint64_t bitmap_phys = (uint64_t)bitmap - hhdm_offset;
    for (uint64_t f = bitmap_phys / PAGE_SIZE;
         f < (bitmap_phys + bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE; f++) {
        bit_set(f);
        free_frames--;
    }

    /* frame 0 stays ours forever so phys addr 0 can mean "nope" */
    if (bitmap_frames > 0 && !bit_test(0)) {
        bit_set(0);
        free_frames--;
    }

    search_hint = 0;
}

void pmm_init(void) {
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        panic("limine kept the memory map to itself");
    }
    kprintf("memory map, as declared by limine:\n");
    pmm_init_from_map(memmap_request.response->entries,
                      memmap_request.response->entry_count,
                      hhdm_request.response->offset);
    kprintf("  -> %lu MiB usable, %lu KiB spent on the bitmap\n",
            pmm_total_bytes() / (1024 * 1024),
            (bitmap_frames / 8) / 1024);
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0 || count > bitmap_frames) {
        return 0;
    }

    /* dumb linear scan for a run of free bits, starting at the hint.
     * two passes: hint..end, then 0..hint, so freed low memory gets
     * found again. o(n) and proud of it */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? search_hint : 0;
        uint64_t end   = pass == 0 ? bitmap_frames : search_hint;
        uint64_t run = 0;
        for (uint64_t f = start; f < end; f++) {
            if (bit_test(f)) {
                run = 0;
                continue;
            }
            run++;
            if (run == count) {
                uint64_t first = f - count + 1;
                for (uint64_t i = first; i <= f; i++) {
                    bit_set(i);
                }
                free_frames -= count;
                search_hint = f + 1;
                return first * PAGE_SIZE;
            }
        }
    }
    return 0;   /* the well is dry */
}

void pmm_free_pages(uint64_t phys, size_t count) {
    if (phys % PAGE_SIZE != 0) {
        panic("pmm_free_pages: %016lx is not page aligned, what is this", phys);
    }
    uint64_t first = phys / PAGE_SIZE;
    for (uint64_t f = first; f < first + count; f++) {
        if (f >= bitmap_frames || !bit_test(f)) {
            panic("pmm: freeing frame %016lx which was never thine to free",
                  f * PAGE_SIZE);
        }
        bit_clear(f);
        free_frames++;
    }
    if (first < search_hint) {
        search_hint = first;
    }
}

uint64_t pmm_alloc(void)         { return pmm_alloc_pages(1); }
void     pmm_free(uint64_t phys) { pmm_free_pages(phys, 1); }

void *pmm_phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

uint64_t pmm_total_bytes(void) { return total_frames * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)  { return free_frames * PAGE_SIZE; }
uint64_t pmm_used_bytes(void)  { return (total_frames - free_frames) * PAGE_SIZE; }
