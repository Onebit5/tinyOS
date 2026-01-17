#include "mm/slab.h"
#include "mm/pmm.h"
#include "lib/panic.h"
#include "cpu/interrupts.h"
#include <stdbool.h>

#define SLAB_MAGIC 0x51ab7a11ed900dull

/* sits at the top of every page a cache owns. the objects follow it */
struct slab {
    uint64_t magic;
    struct slab_cache *cache;
    struct slab *next, *prev;
    void *free;                 /* free list, threaded through the objects */
    uint32_t in_use;
    uint32_t total;
};

#define SLAB_HEADER ((sizeof(struct slab) + 15) & ~15ull)

static struct slab_cache *all_caches;

static struct slab *page_of(const void *object) {
    return (struct slab *)((uintptr_t)object & ~(uintptr_t)(PAGE_SIZE - 1));
}

void slab_cache_init(struct slab_cache *cache, const char *name,
                     size_t obj_size) {
    if (cache->obj_size != 0) {
        return;             /* already set up */
    }

    size_t size = (obj_size + 15) & ~15ull;
    if (size < 16) {
        size = 16;
    }
    if (size > PAGE_SIZE - SLAB_HEADER) {
        panic("slab: %s wants %zu bytes, which does not fit in a page",
              name, obj_size);
    }

    cache->name = name;
    cache->obj_size = size;
    cache->per_slab = (PAGE_SIZE - SLAB_HEADER) / size;
    cache->partial = NULL;
    cache->full = NULL;
    cache->pages = 0;
    cache->in_use = 0;
    cache->high_water = 0;

    cache->next = all_caches;
    all_caches = cache;
}

/* ---- the two lists ------------------------------------------------- */

static void list_insert(struct slab **list, struct slab *s) {
    s->prev = NULL;
    s->next = *list;
    if (*list != NULL) {
        (*list)->prev = s;
    }
    *list = s;
}

static void list_remove(struct slab **list, struct slab *s) {
    if (s->prev != NULL) {
        s->prev->next = s->next;
    } else {
        *list = s->next;
    }
    if (s->next != NULL) {
        s->next->prev = s->prev;
    }
    s->next = s->prev = NULL;
}

/* ---- growing and shrinking ----------------------------------------- */

static struct slab *new_slab(struct slab_cache *cache) {
    uint64_t phys = pmm_alloc();
    if (phys == 0) {
        return NULL;
    }

    struct slab *s = pmm_phys_to_virt(phys);
    s->magic = SLAB_MAGIC;
    s->cache = cache;
    s->in_use = 0;
    s->total = (uint32_t)cache->per_slab;
    s->free = NULL;

    /* thread a free list through every object. backwards, so the list
     * comes out in address order and the first few allocations from a
     * fresh page are neighbours */
    uint8_t *base = (uint8_t *)s + SLAB_HEADER;
    for (size_t i = cache->per_slab; i > 0; i--) {
        void *object = base + (i - 1) * cache->obj_size;
        *(void **)object = s->free;
        s->free = object;
    }

    cache->pages++;
    list_insert(&cache->partial, s);
    return s;
}

/* ---- the two calls that matter ------------------------------------- */

void *slab_alloc(struct slab_cache *cache) {
    uint64_t flags = irq_save();

    struct slab *s = cache->partial;
    if (s == NULL) {
        s = new_slab(cache);
        if (s == NULL) {
            irq_restore(flags);
            return NULL;
        }
    }

    void *object = s->free;
    s->free = *(void **)object;
    s->in_use++;

    /* a page with nothing left moves off the partial list, so the next
     * allocation does not have to look at it and find it wanting */
    if (s->free == NULL) {
        list_remove(&cache->partial, s);
        list_insert(&cache->full, s);
    }

    cache->in_use++;
    if (cache->in_use > cache->high_water) {
        cache->high_water = cache->in_use;
    }

    irq_restore(flags);
    return object;
}

void slab_free(void *object) {
    if (object == NULL) {
        return;
    }

    struct slab *s = page_of(object);
    if (s->magic != SLAB_MAGIC) {
        panic("slab_free: %p came from no cache of mine", object);
    }

    struct slab_cache *cache = s->cache;
    uint64_t flags = irq_save();

    if (s->in_use == 0) {
        panic("slab_free: %p returned to %s twice over", object, cache->name);
    }

    bool was_full = (s->free == NULL);

    *(void **)object = s->free;
    s->free = object;
    s->in_use--;
    cache->in_use--;

    if (was_full) {
        list_remove(&cache->full, s);
        list_insert(&cache->partial, s);
    }

    /* a page nobody is using goes back to the pmm. holding on to it
     * would be faster next time, but a kernel that never gives memory
     * back is a kernel that eventually has none */
    if (s->in_use == 0) {
        list_remove(&cache->partial, s);
        s->magic = 0;
        cache->pages--;
        uint64_t phys = (uint64_t)s - pmm_hhdm_offset();
        irq_restore(flags);
        pmm_free(phys);
        return;
    }

    irq_restore(flags);
}

int slab_owns(const void *object) {
    if (object == NULL) {
        return 0;
    }
    return page_of(object)->magic == SLAB_MAGIC;
}

struct slab_cache *slab_first_cache(void) {
    return all_caches;
}
