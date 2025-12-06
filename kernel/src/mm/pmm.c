#include "mm/pmm.h"
#include "mm/buddy.h"
#include "lib/kprintf.h"
#include "cpu/interrupts.h"
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
static uint64_t managed_frames;  /* how many frames the allocator covers */
static uint64_t total_frames;    /* usable frames overall */
static uint64_t highest_addr;    /* top of the direct map, see pmm.h */
static uint64_t peak_used;       /* the high-water mark, in frames */
static uint64_t meta_bytes;      /* what the buddy's bookkeeping costs */

/* the frames the bookkeeping itself lives in, so we know not to give
 * them away. an allocator that hands out its own records is briefly
 * very fast and then very confused */
static uint64_t meta_first, meta_last;

/* limine's own memory, noted down at init because the memmap we would
 * otherwise read it from is itself sitting in that memory */
#define MAX_RECLAIM 16
static struct { uint64_t base, length; } reclaim[MAX_RECLAIM];
static size_t reclaim_count;

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

/* the buddy threads its free lists through the free pages themselves,
 * so it needs a way to reach one. through the hhdm, same as everything */
static void *frame_to_virt(uint64_t frame) {
    return (void *)(frame * PAGE_SIZE + hhdm_offset);
}

/* hand [first, last) to the allocator, minus the two things that are
 * never anyone's to allocate: frame 0, so that a physical address of 0
 * can safely mean "no", and whatever the bookkeeping sits in. returns
 * how many frames actually went in */
static uint64_t give_away(uint64_t first, uint64_t last) {
    if (first == 0) {
        first = 1;
    }
    if (last > managed_frames) {
        last = managed_frames;
    }
    if (first >= last) {
        return 0;
    }

    /* if the metadata is somewhere in the middle, give away the two
     * halves on either side of it. neither of those overlaps it, so
     * this recurses exactly one level deep */
    if (meta_first < last && meta_last > first) {
        uint64_t added = 0;
        if (meta_first > first) {
            added += give_away(first, meta_first);
        }
        if (meta_last < last) {
            added += give_away(meta_last, last);
        }
        return added;
    }

    buddy_add_range(first, last - first);
    return last - first;
}

void pmm_init_from_map(struct limine_memmap_entry **entries, size_t count,
                       uint64_t hhdm) {
    hhdm_offset = hhdm;

    /* pass 1: how far up does usable ram go, and how much is there */
    uint64_t highest = 0;
    total_frames = 0;
    highest_addr = 0;
    reclaim_count = 0;
    peak_used = 0;
    for (size_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = entries[i];
        kprintf("  %016lx - %016lx  %s\n", e->base, e->base + e->length,
                memmap_type_name(e->type));
        if (e->type != LIMINE_MEMMAP_RESERVED
            && e->type != LIMINE_MEMMAP_BAD_MEMORY
            && e->base + e->length > highest_addr) {
            highest_addr = e->base + e->length;
        }
        if (e->type == LIMINE_MEMMAP_USABLE) {
            total_frames += e->length / PAGE_SIZE;
        }
        /* the allocator has to cover limine's memory too, or we would
         * have nowhere to record those frames when we reclaim them
         * later -- they sit above the last usable region on most
         * machines */
        if (e->type == LIMINE_MEMMAP_USABLE
            || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            if (e->base + e->length > highest) {
                highest = e->base + e->length;
            }
        }
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            && reclaim_count < MAX_RECLAIM) {
            reclaim[reclaim_count].base = e->base;
            reclaim[reclaim_count].length = e->length;
            reclaim_count++;
        }
    }

    managed_frames = highest / PAGE_SIZE;
    meta_bytes = buddy_metadata_bytes(managed_frames);

    /* pass 2: find a usable region big enough to park the bookkeeping */
    void *meta = NULL;
    for (size_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= meta_bytes) {
            meta = (void *)(e->base + hhdm_offset);
            meta_first = e->base / PAGE_SIZE;
            meta_last  = (e->base + meta_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
            break;
        }
    }
    if (meta == NULL) {
        panic("nowhere to put the page allocator's books (%lu bytes). "
              "how little ram is this?", meta_bytes);
    }

    buddy_init(managed_frames, meta, frame_to_virt);

    /* pass 3: everything starts taken, and the usable regions are given
     * away one at a time */
    for (size_t i = 0; i < count; i++) {
        struct limine_memmap_entry *e = entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        give_away(e->base / PAGE_SIZE, (e->base + e->length) / PAGE_SIZE);
    }
}

void pmm_init(void) {
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        panic("limine kept the memory map to itself");
    }
    kprintf("memory map, as declared by limine:\n");
    pmm_init_from_map(memmap_request.response->entries,
                      memmap_request.response->entry_count,
                      hhdm_request.response->offset);
    kprintf("  -> %lu MiB usable, %lu KiB spent on the buddy's books\n",
            pmm_total_bytes() / (1024 * 1024), meta_bytes / 1024);
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0) {
        return 0;
    }

    /* the buddy rounds up to a power of two, so five pages costs eight.
     * a request too large for the biggest block gets an order it will
     * refuse rather than a block that is quietly too small */
    unsigned order = buddy_order_for(count);

    /* threads can be preempted mid-list and the allocator would hand
     * the same block to two of them */
    uint64_t flags = irq_save();

    uint64_t frame = buddy_alloc(order);
    if (frame == BUDDY_NO_BLOCK) {
        irq_restore(flags);
        return 0;       /* the well is dry */
    }

    uint64_t used = total_frames - buddy_free_frames();
    if (used > peak_used) {
        peak_used = used;
    }

    irq_restore(flags);
    return frame * PAGE_SIZE;
}

void pmm_free_pages(uint64_t phys, size_t count) {
    if (phys % PAGE_SIZE != 0) {
        panic("pmm_free_pages: %016lx is not page aligned, what is this", phys);
    }
    if (count == 0) {
        return;
    }

    uint64_t frame = phys / PAGE_SIZE;
    unsigned order = buddy_order_for(count);

    uint64_t flags = irq_save();
    if (frame >= managed_frames || buddy_is_free_block(frame, order)) {
        panic("pmm: freeing frame %016lx which was never thine to free", phys);
    }
    buddy_free(frame, order);
    irq_restore(flags);
}

uint64_t pmm_alloc(void)         { return pmm_alloc_pages(1); }
void     pmm_free(uint64_t phys) { pmm_free_pages(phys, 1); }

void *pmm_phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

uint64_t pmm_reclaim_bootloader(void) {
    uint64_t flags = irq_save();
    uint64_t gained = 0;

    for (size_t i = 0; i < reclaim_count; i++) {
        uint64_t first = (reclaim[i].base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t last  = (reclaim[i].base + reclaim[i].length) / PAGE_SIZE;

        uint64_t added = give_away(first, last);
        total_frames += added;      /* it counts as ram from now on */
        gained += added * PAGE_SIZE;
    }

    /* forget the ranges, so a second call cant double free them */
    reclaim_count = 0;

    irq_restore(flags);
    return gained;
}

bool pmm_translate_is_tracked(uint64_t phys) {
    return phys / PAGE_SIZE < managed_frames;
}

uint64_t pmm_hhdm_offset(void)     { return hhdm_offset; }
uint64_t pmm_highest_address(void) { return highest_addr; }

uint64_t pmm_metadata_bytes(void)  { return meta_bytes; }
uint64_t pmm_blocks_at(unsigned order) { return buddy_blocks_at(order); }

uint64_t pmm_peak_bytes(void)  { return peak_used * PAGE_SIZE; }
uint64_t pmm_total_bytes(void) { return total_frames * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)  { return buddy_free_frames() * PAGE_SIZE; }
uint64_t pmm_used_bytes(void)  {
    return (total_frames - buddy_free_frames()) * PAGE_SIZE;
}
