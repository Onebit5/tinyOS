#ifndef MM_ADDRSPACE_H
#define MM_ADDRSPACE_H

#include <stdint.h>
#include <stdbool.h>

/* a private view of memory.
 *
 * up to now there was one set of page tables and every program loaded
 * into it, which meant they linked to the same addresses and only one
 * could exist at a time. now each gets its own pml4, and the difference
 * between them is only the lower half -- the upper half, where the
 * kernel and the direct map live, is shared by reference so the kernel
 * is reachable no matter whose tables are loaded. it has to be: the
 * stack I am standing on when I switch is up there. */

/* ---- regions ---------------------------------------------------------
 *
 * a page fault stopped being purely an error in 0.2.11, when it could
 * mean "copy this page". this is the other thing it can mean: "there
 * was never a page here yet, and there was always going to be one".
 *
 * the difference between that and a wild pointer is a *record* saying
 * which addresses are legitimately empty. without one there is no way
 * to tell a stack that wants to grow from a program dereferencing
 * nonsense, and a kernel that guesses wrong either kills good programs
 * or conjures memory for bad ones.
 *
 * so each space carries a small table of the ranges it has agreed to,
 * and every not-present fault is answered out of it or not at all. */

#define VMA_MAX 16

enum vma_kind {
    VMA_NONE = 0,
    VMA_ANON,       /* zeroes. a stack, or something mmap handed out */
    VMA_FILE,       /* bytes out of an image that is already in memory */
};

struct vma {
    enum vma_kind kind;
    uint64_t start, end;        /* [start, end), page aligned */
    uint64_t flags;             /* what a page here is allowed to be */

    /* for a file-backed one: where the bytes come from, and where they
     * stop. past `file_end` the page is zeroes, which is how the same
     * record describes a segment and the bss hanging off the end of it */
    const uint8_t *image;
    uint64_t image_offset;      /* offset in the image of `start` */
    uint64_t file_end;          /* last address with a byte behind it */
};

struct addrspace {
    uint64_t pml4;      /* physical address of the top level table */

    struct vma vmas[VMA_MAX];
};

/* agree to a range of addresses without mapping any of it. returns
 * false if there is no room in the table or the range overlaps one
 * already there -- two records disagreeing about one address is worse
 * than refusing to make the second */
bool addrspace_add_region(struct addrspace *as, uint64_t start, uint64_t end,
                          uint64_t flags, enum vma_kind kind,
                          const uint8_t *image, uint64_t image_offset,
                          uint64_t file_end);

/* take one back, unmapping and freeing whatever of it was ever
 * populated. false if there is no region starting exactly there --
 * partial unmapping is a thing unix does and nothing here needs */
bool addrspace_drop_region(struct addrspace *as, uint64_t start);

/* somewhere `len` bytes will fit, as a region of anonymous memory.
 * returns the address, or 0. this is the whole of mmap */
uint64_t addrspace_reserve(struct addrspace *as, uint64_t len, uint64_t flags);

/* a new space with the kernel half already visible and nothing else.
 * the kernel's own pml4 is passed in rather than fetched, which is the
 * same reason pmm_init_from_map takes a memory map: it lets the whole
 * thing be built and inspected on a host with no cpu involved */
struct addrspace *addrspace_create(uint64_t kernel_pml4);

/* a copy of `from` that shares every page with it rather than copying
 * any.
 *
 * every writable page in both spaces is marked read-only and flagged
 * PTE_COW, and the frame gains a second holder. the first write on
 * either side faults, and the handler makes that one page private. so
 * forking costs the page *tables* and nothing else, which for a program
 * that immediately goes off and does something different is nearly all
 * of the saving there is.
 *
 * the tables themselves really are copied. they have to be: the two
 * spaces are about to disagree about what is in them */
struct addrspace *addrspace_fork(const struct addrspace *from,
                                 uint64_t kernel_pml4);

/* a fault. one of two things, and the difference is `present`:
 *
 *   a write to a page that is there but marked copy-on-write. give this
 *   space a private copy and let the write through.
 *
 *   a touch of a page that is not there at all. if the address is
 *   inside a region this space agreed to, make one; otherwise it is a
 *   wild pointer and always was.
 *
 * returns false if it was neither, which means the fault was a real one
 * and the caller should treat it as such */
bool addrspace_fault(struct addrspace *as, uint64_t virt, bool write,
                     bool present);

/* free everything in the lower half -- the program's pages, its stack,
 * and the tables that described them. the upper half is shared and is
 * emphatically not mine to free.
 *
 * only safe once nothing is running on it. the reaper does this, from
 * another thread, after the switch away has already happened */
void addrspace_destroy(struct addrspace *as);

/* make this space the live one. passing NULL means the kernel's own.
 * writing cr3 flushes the whole tlb, so this does nothing when the
 * space is already loaded */
void addrspace_switch(struct addrspace *as);

/* how many frames a space is holding, for `ps` to report */
uint64_t addrspace_frames(struct addrspace *as);

#endif
