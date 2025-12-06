#ifndef MM_SLAB_H
#define MM_SLAB_H

#include <stdint.h>
#include <stddef.h>

/* object caches, for the things a kernel allocates over and over.
 *
 * a general heap has to ask "how big?" and "where does it fit?" every
 * single time. but a kernel spends most of its allocations on a handful
 * of structs whose size never changes -- a thread, a process, an
 * address space -- and for those both questions have the same answer
 * every time. a slab cache answers them once: take a page, cut it into
 * objects of exactly that size, and thread a free list through the ones
 * nobody is using. allocation becomes taking the head of a list.
 *
 * each page carries a small header saying which cache it belongs to and
 * how many of its objects are still out. that header is what lets
 * slab_free take a bare pointer and work out where it came from -- mask
 * off the low twelve bits and the answer is right there -- and it is
 * also what lets a page whose objects have all come home be given back
 * to the pmm rather than held forever. */

struct slab;

struct slab_cache {
    const char *name;
    size_t obj_size;            /* rounded up to 16 */
    size_t per_slab;            /* objects that fit in one page */

    struct slab *partial;       /* pages with at least one object free */
    struct slab *full;          /* pages with none */

    size_t pages;               /* how many pages this cache is holding */
    size_t in_use;              /* objects currently handed out */
    size_t high_water;          /* the most that were ever out at once */

    struct slab_cache *next;    /* every cache, so `slabs` can list them */
};

/* set a cache up. the struct is the caller's -- usually a static in
 * whichever file allocates the thing -- so no chicken-and-egg with the
 * heap. safe to call twice on the same cache; the second is ignored */
void slab_cache_init(struct slab_cache *cache, const char *name,
                     size_t obj_size);

void *slab_alloc(struct slab_cache *cache);

/* note there is no cache argument: the page says which cache it is.
 * that is the whole reason for the header */
void slab_free(void *object);

/* is this pointer the start of a live slab object? kfree uses it to
 * tell a slab allocation from a whole-page one */
int slab_owns(const void *object);

/* walk the registered caches, for the shell */
struct slab_cache *slab_first_cache(void);

#endif
