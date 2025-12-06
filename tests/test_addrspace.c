/* per-process address spaces: what they share, what they own, and
 * whether letting one go really hands everything back.
 *
 * a leak here grows every time a program runs, and a double free is
 * worse -- so the interesting assertion is that the pmm's books are
 * exactly level after a space is created, used and destroyed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#define ARENA_PAGES 8192
static uint8_t *arena;
static uint8_t taken[ARENA_PAGES];
static uint64_t next_free = 1;
static uint64_t outstanding;            /* frames handed out and not returned */

uint64_t pmm_alloc_pages(size_t count) {
    if (next_free + count > ARENA_PAGES) return 0;
    uint64_t phys = next_free * 4096;
    for (size_t i = 0; i < count; i++) {
        if (taken[next_free + i]) { printf("FAIL: pmm handed out a live frame\n"); exit(1); }
        taken[next_free + i] = 1;
    }
    next_free += count;
    outstanding += count;
    return phys;
}
void pmm_free_pages(uint64_t phys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t idx = phys / 4096 + i;
        if (idx >= ARENA_PAGES || !taken[idx]) {
            printf("FAIL: freed frame %#lx which was not allocated (double free?)\n",
                   (unsigned long)(idx * 4096));
            exit(1);
        }
        taken[idx] = 0;
    }
    outstanding -= count;
}
void *pmm_phys_to_virt(uint64_t phys) { return arena + phys; }
uint64_t pmm_alloc(void)         { return pmm_alloc_pages(1); }
void     pmm_free(uint64_t phys) { pmm_free_pages(phys, 1); }
/* the slab caches turn an object pointer back into a physical address
 * by subtracting this, so it has to be where the arena really is */
uint64_t pmm_hhdm_offset(void)   { return (uint64_t)arena; }
void kprintf(const char *f, ...) { (void)f; }
void panic(const char *f, ...) { (void)f; printf("PANIC\n"); exit(1); }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

#include "mm/vmm.h"
#include "mm/addrspace.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static uint64_t kernel_space;

/* addrspace_switch is the only part that touches a real cpu, so the
 * kernel build keeps it and the host build supplies this instead */
void addrspace_switch(struct addrspace *as) { (void)as; }

int main(void) {
    arena = aligned_alloc(4096, (size_t)ARENA_PAGES * 4096);
    memset(arena, 0, (size_t)ARENA_PAGES * 4096);

    /* a stand-in kernel: a direct map high up, and the kernel image */
    kernel_space = vmm_new_address_space();
    vmm_map_range(kernel_space, 0xffff800000000000ull, 0, 4 * 1024 * 1024,
                  PTE_WRITE | PTE_NX);
    vmm_map_range(kernel_space, 0xffffffff80000000ull, 0x100000, 0x8000, 0);

    /* ---- what a new space shares ---- */
    struct addrspace *a = addrspace_create(kernel_space);
    CHECK(a != NULL && a->pml4 != 0, "a space is born");

    CHECK(vmm_translate(a->pml4, 0xffff800000000000ull) == 0,
          "the direct map is visible in it");
    CHECK(vmm_translate(a->pml4, 0xffffffff80001000ull) == 0x101000,
          "and so is the kernel image -- it must be, the stack is up there");
    CHECK(vmm_translate(a->pml4, 0x400000) == VMM_NO_MAPPING,
          "but the lower half starts empty");

    /* shared by reference, not by copy: a change the kernel makes later
     * has to be visible in spaces that already exist */
    uint64_t late = pmm_alloc_pages(1);
    vmm_map_range(kernel_space, 0xffff800000400000ull, late, 4096, PTE_WRITE);
    CHECK(vmm_translate(a->pml4, 0xffff800000400000ull) == late,
          "a later kernel mapping shows up in a space made before it");

    addrspace_destroy(a);

    /* everything outstanding now belongs to the kernel and nothing
     * else, which makes it the reference the leak check needs */
    uint64_t kernel_only = outstanding;

    /* ---- two spaces, same addresses, different memory ----
     * the frames come from the pmm the way elf_load gets them, so
     * destroying a space hands back exactly what was taken */
    struct addrspace *x = addrspace_create(kernel_space);
    struct addrspace *y = addrspace_create(kernel_space);

    uint64_t page_x = pmm_alloc_pages(1);
    uint64_t page_y = pmm_alloc_pages(1);
    vmm_map_range(x->pml4, 0x400000, page_x, 4096, PTE_USER | PTE_WRITE);
    vmm_map_range(y->pml4, 0x400000, page_y, 4096, PTE_USER | PTE_WRITE);

    CHECK(vmm_translate(x->pml4, 0x400000) == page_x, "x sees its own page");
    CHECK(vmm_translate(y->pml4, 0x400000) == page_y, "y sees a different one");
    CHECK(vmm_translate(x->pml4, 0x400000) != vmm_translate(y->pml4, 0x400000),
          "the same address means different memory in each -- the whole point");

    CHECK(addrspace_frames(x) == 1, "x is holding one page");
    uint64_t more = pmm_alloc_pages(4);
    vmm_map_range(x->pml4, 0x800000, more, 4 * 4096, PTE_USER | PTE_WRITE);
    CHECK(addrspace_frames(x) == 5, "and five after a bigger mapping");
    CHECK(addrspace_frames(y) == 1, "without disturbing y's count");

    /* ---- letting one go ---- */
    uint64_t before = outstanding;
    addrspace_destroy(y);
    CHECK(outstanding < before, "destroying a space returns frames");

    CHECK(vmm_translate(x->pml4, 0x400000) == page_x,
          "x still works after y was destroyed");
    CHECK(vmm_translate(x->pml4, 0xffff800000000000ull) == 0,
          "and the shared kernel half was not freed with y");

    addrspace_destroy(x);
    CHECK(outstanding == kernel_only,
          "with both spaces gone the pmm is exactly level again -- nothing "
          "leaked, and nothing shared was taken with them");

    /* the kernel's own mappings must have survived all of that */
    CHECK(vmm_translate(kernel_space, 0xffff800000000000ull) == 0,
          "the kernel direct map is intact");
    CHECK(vmm_translate(kernel_space, 0xffffffff80001000ull) == 0x101000,
          "and so is the kernel image");
    CHECK(vmm_translate(kernel_space, 0xffff800000400000ull) == late,
          "including the mapping made while spaces existed");

    /* ---- a space with nothing in it ---- */
    before = outstanding;
    struct addrspace *empty = addrspace_create(kernel_space);
    addrspace_destroy(empty);
    CHECK(outstanding == before, "an unused space costs nothing once freed");

    if (!failures) printf("all good\n");
    return failures;
}
