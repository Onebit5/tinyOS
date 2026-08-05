#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "lib/panic.h"
#include "cpu/interrupts.h"

#define KMALLOC_MAGIC   0xa110c8ed
#define CHUNK_MIN_PAGES 4           /* dont bother the pmm for crumbs */

/* every block, free or not, carries one of these. size is the whole
 * block including the header. blocks are chained in address order,
 * which is what makes coalescing a simple neighbour check */
struct block {
    uint64_t size;
    struct block *next;
    uint32_t magic;
    uint32_t free;
} __attribute__((aligned(16)));

static struct block *head;
static uint64_t heap_total;
static uint64_t heap_used;

#define ALIGN16(x) (((x) + 15) & ~15ull)

/* grab a fresh contiguous chunk from the pmm and slot it into the list
 * at its address-ordered spot. returns the new block or NULL when the
 * pmm says no */
static struct block *grow(uint64_t need) {
    size_t pages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < CHUNK_MIN_PAGES) {
        pages = CHUNK_MIN_PAGES;
    }

    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        return NULL;
    }

    struct block *b = pmm_phys_to_virt(phys);
    b->size = pages * PAGE_SIZE;
    b->magic = KMALLOC_MAGIC;
    b->free = 1;
    heap_total += b->size;

    if (head == NULL || b < head) {
        b->next = head;
        head = b;
    } else {
        struct block *cur = head;
        while (cur->next && cur->next < b) {
            cur = cur->next;
        }
        b->next = cur->next;
        cur->next = b;
    }
    return b;
}

/* merge every pair of address-adjacent free blocks. blocks from
 * different pmm chunks are never adjacent so they never merge, which
 * is exactly right */
static void coalesce(void) {
    for (struct block *cur = head; cur && cur->next; ) {
        if (cur->free && cur->next->free
            && (uint8_t *)cur + cur->size == (uint8_t *)cur->next) {
            cur->size += cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

static void *carve(struct block *b, uint64_t need) {
    /* split if whats left is worth keeping as its own block */
    if (b->size - need >= sizeof(struct block) + 16) {
        struct block *rest = (struct block *)((uint8_t *)b + need);
        rest->size = b->size - need;
        rest->next = b->next;
        rest->magic = KMALLOC_MAGIC;
        rest->free = 1;
        b->size = need;
        b->next = rest;
    }
    b->free = 0;
    heap_used += b->size;
    return (uint8_t *)b + sizeof(struct block);
}

/* the free list is walked and rewritten on every call, so a thread
 * preempted halfway through would hand the next one a heap in pieces.
 * interrupts off for the duration -- these are short */

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    uint64_t need = ALIGN16(size) + sizeof(struct block);
    uint64_t flags = irq_save();

    for (struct block *cur = head; cur; cur = cur->next) {
        if (cur->free && cur->size >= need) {
            void *p = carve(cur, need);
            irq_restore(flags);
            return p;
        }
    }

    struct block *fresh = grow(need);
    if (fresh == NULL) {
        irq_restore(flags);
        return NULL;    /* memory hath forsaken us */
    }
    void *p = carve(fresh, need);
    irq_restore(flags);
    return p;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    struct block *b = (struct block *)((uint8_t *)ptr - sizeof(struct block));
    if (b->magic != KMALLOC_MAGIC) {
        panic("kfree: %p knows not this heap. whence came it?", ptr);
    }

    uint64_t flags = irq_save();
    if (b->free) {
        panic("kfree: %p hath already been returned. a bond broken twice", ptr);
    }
    b->free = 1;
    heap_used -= b->size;
    coalesce();
    irq_restore(flags);
}

uint64_t kheap_total_bytes(void) { return heap_total; }
uint64_t kheap_used_bytes(void)  { return heap_used; }
