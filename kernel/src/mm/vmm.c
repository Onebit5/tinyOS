#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"

/* index into each level of the tables. nine bits each, which is why a
 * table is 512 entries and a page is 4KiB */
#define PML4_IDX(v) (((v) >> 39) & 0x1ff)
#define PDPT_IDX(v) (((v) >> 30) & 0x1ff)
#define PD_IDX(v)   (((v) >> 21) & 0x1ff)
#define PT_IDX(v)   (((v) >> 12) & 0x1ff)

static uint64_t kernel_pml4;

uint64_t vmm_kernel_pml4(void) {
    return kernel_pml4;
}

static uint64_t *table_at(uint64_t phys) {
    return pmm_phys_to_virt(phys);
}

uint64_t vmm_new_address_space(void) {
    uint64_t phys = pmm_alloc_pages(1);
    if (phys == 0) {
        return 0;
    }
    memset(table_at(phys), 0, PAGE_SIZE);
    return phys;
}

/* walk one level down, optionally building the next table on the way.
 * returns NULL if theres nothing there (or if a huge page is in the
 * way, which the callers treat as "somebody already mapped this") */
static uint64_t *step(uint64_t *table, size_t idx, bool create) {
    if (table[idx] & PTE_PRESENT) {
        if (table[idx] & PTE_HUGE) {
            return NULL;
        }
        return table_at(table[idx] & PTE_ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }

    uint64_t phys = pmm_alloc_pages(1);
    if (phys == 0) {
        return NULL;
    }
    memset(table_at(phys), 0, PAGE_SIZE);

    /* intermediate entries stay permissive on purpose. the cpu ANDs the
     * write bit and ORs the nx bit down the whole chain, so if we were
     * strict up here the leaf could never grant anything */
    table[idx] = phys | PTE_PRESENT | PTE_WRITE;
    return table_at(phys);
}

static bool map_4k(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *t = table_at(pml4);

    t = step(t, PML4_IDX(virt), true); if (!t) return false;
    t = step(t, PDPT_IDX(virt), true); if (!t) return false;
    t = step(t, PD_IDX(virt),   true); if (!t) return false;

    t[PT_IDX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    return true;
}

static bool map_2m(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *t = table_at(pml4);

    t = step(t, PML4_IDX(virt), true); if (!t) return false;
    t = step(t, PDPT_IDX(virt), true); if (!t) return false;

    t[PD_IDX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT | PTE_HUGE;
    return true;
}

bool vmm_map_range(uint64_t pml4, uint64_t virt, uint64_t phys,
                   uint64_t size, uint64_t flags) {
    /* round the window outward so a request never ends up half mapped */
    uint64_t end = (virt + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    virt &= ~(PAGE_SIZE - 1);
    phys &= ~(PAGE_SIZE - 1);

    while (virt < end) {
        /* a 2MiB page needs both addresses aligned to it and enough
         * left to fill, otherwise fall back to 4KiB */
        bool can_huge = (virt % PAGE_SIZE_2M) == 0
                     && (phys % PAGE_SIZE_2M) == 0
                     && (end - virt) >= PAGE_SIZE_2M;

        if (can_huge) {
            if (!map_2m(pml4, virt, phys, flags)) {
                return false;
            }
            virt += PAGE_SIZE_2M;
            phys += PAGE_SIZE_2M;
        } else {
            if (!map_4k(pml4, virt, phys, flags)) {
                return false;
            }
            virt += PAGE_SIZE;
            phys += PAGE_SIZE;
        }
    }
    return true;
}

/* the software walk. this is what lets us check our work before
 * trusting it to cr3, and what backs the shell's `vmm` command */
static uint64_t *walk_to_leaf(uint64_t pml4, uint64_t virt, uint64_t *out_entry,
                              bool *out_huge) {
    uint64_t *t = table_at(pml4);
    size_t idx = PML4_IDX(virt);

    if (!(t[idx] & PTE_PRESENT)) return NULL;
    t = table_at(t[idx] & PTE_ADDR_MASK);

    idx = PDPT_IDX(virt);
    if (!(t[idx] & PTE_PRESENT)) return NULL;
    if (t[idx] & PTE_HUGE) {    /* 1GiB page, we never make these but be safe */
        *out_entry = t[idx];
        *out_huge = true;
        return &t[idx];
    }
    t = table_at(t[idx] & PTE_ADDR_MASK);

    idx = PD_IDX(virt);
    if (!(t[idx] & PTE_PRESENT)) return NULL;
    if (t[idx] & PTE_HUGE) {
        *out_entry = t[idx];
        *out_huge = true;
        return &t[idx];
    }
    t = table_at(t[idx] & PTE_ADDR_MASK);

    idx = PT_IDX(virt);
    if (!(t[idx] & PTE_PRESENT)) return NULL;
    *out_entry = t[idx];
    *out_huge = false;
    return &t[idx];
}

uint64_t vmm_translate(uint64_t pml4, uint64_t virt) {
    uint64_t entry = 0;
    bool huge = false;
    if (walk_to_leaf(pml4, virt, &entry, &huge) == NULL) {
        return VMM_NO_MAPPING;
    }
    if (huge) {
        return (entry & PTE_ADDR_MASK & ~(PAGE_SIZE_2M - 1))
             | (virt & (PAGE_SIZE_2M - 1));
    }
    return (entry & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

uint64_t vmm_flags(uint64_t pml4, uint64_t virt) {
    uint64_t entry = 0;
    bool huge = false;
    if (walk_to_leaf(pml4, virt, &entry, &huge) == NULL) {
        return 0;
    }
    return entry & ~PTE_ADDR_MASK;
}

/* turn one 2MiB page into 512 4KiB ones covering exactly the same
 * memory with exactly the same flags. nothing observable changes,
 * which is the point -- it just makes the map fine grained enough to
 * poke a hole in */
static bool split_2m(uint64_t *pd_entry) {
    uint64_t old = *pd_entry;
    uint64_t base = old & PTE_ADDR_MASK & ~(PAGE_SIZE_2M - 1);
    uint64_t flags = old & ~PTE_ADDR_MASK & ~PTE_HUGE;

    uint64_t pt_phys = pmm_alloc_pages(1);
    if (pt_phys == 0) {
        return false;
    }

    uint64_t *pt = table_at(pt_phys);
    for (size_t i = 0; i < 512; i++) {
        pt[i] = (base + i * PAGE_SIZE) | flags;
    }

    *pd_entry = pt_phys | PTE_PRESENT | PTE_WRITE;
    return true;
}

bool vmm_unmap_page(uint64_t pml4, uint64_t virt) {
    uint64_t *t = table_at(pml4);
    size_t idx = PML4_IDX(virt);

    if (!(t[idx] & PTE_PRESENT)) return false;
    t = table_at(t[idx] & PTE_ADDR_MASK);

    idx = PDPT_IDX(virt);
    if (!(t[idx] & PTE_PRESENT)) return false;
    t = table_at(t[idx] & PTE_ADDR_MASK);

    idx = PD_IDX(virt);
    if (!(t[idx] & PTE_PRESENT)) return false;
    if (t[idx] & PTE_HUGE) {
        if (!split_2m(&t[idx])) {
            return false;
        }
    }
    t = table_at(t[idx] & PTE_ADDR_MASK);

    t[PT_IDX(virt)] = 0;
    return true;
}

/* ---- bringing up the kernel's own address space ---------------------
 * everything below talks to the actual cpu, so the host tests skip it
 * and exercise the portable half above instead */
#ifndef TINYOS_HOSTED

#include "cpu/msr.h"
#include "limine.h"

void kmain(void);   /* just for its address, to check .text is mapped */

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kernel_addr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

/* laid down by linker.ld, one per section boundary */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __kernel_start[], __kernel_end[];

/* PTE_NX if the cpu will honour it, 0 otherwise. setting bit 63 when
 * EFER.NXE is clear is a reserved-bit violation, which faults on every
 * access -- so this has to be a runtime decision, not a constant */
static uint64_t nx = 0;

static void enable_nx(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1u << 20))) {
        kprintf("  !! this cpu has no NX bit, W^X will be W^nothing\n");
        return;
    }
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    nx = PTE_NX;
}

/* without CR0.WP, ring 0 may scribble on read-only pages regardless of
 * what the tables say, which would make the whole exercise decorative */
static void enable_write_protect(void) {
    uint64_t cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ull << 16);
    asm volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static uint64_t read_rsp(void) {
    uint64_t rsp;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

/* check our work before betting the machine on it. a wrong mapping
 * here is a triple fault with no message and no debugger, so anything
 * we can catch in software first is worth catching */
static void require_maps_to(uint64_t pml4, uint64_t virt, uint64_t want,
                            const char *what) {
    uint64_t got = vmm_translate(pml4, virt);
    if (got != want) {
        panic("vmm: %s at %p should reach %p but reaches %p",
              what, (void *)virt, (void *)want, (void *)got);
    }
}

static void require_mapped(uint64_t pml4, uint64_t virt, const char *what) {
    if (vmm_translate(pml4, virt) == VMM_NO_MAPPING) {
        panic("vmm: %s at %p is not mapped -- the switch would kill us",
              what, (void *)virt);
    }
}

static void map_section(uint64_t pml4, char *start, char *end,
                        uint64_t phys_base, uint64_t virt_base,
                        uint64_t flags, const char *name) {
    uint64_t v = (uint64_t)start;
    uint64_t size = (uint64_t)end - v;
    if (size == 0) {
        return;
    }
    uint64_t p = phys_base + (v - virt_base);

    if (!vmm_map_range(pml4, v, p, size, flags)) {
        panic("vmm: ran out of memory mapping %s", name);
    }
    kprintf("  %-7s %p .. %p  %s%s\n", name, (void *)v, (void *)end,
            (flags & PTE_WRITE) ? "rw" : "r-",
            (flags & PTE_NX) ? "-" : "x");
}

void vmm_init(void) {
    if (kernel_addr_request.response == NULL) {
        panic("limine wouldnt say where it put us");
    }

    uint64_t phys_base = kernel_addr_request.response->physical_base;
    uint64_t virt_base = kernel_addr_request.response->virtual_base;
    uint64_t hhdm = pmm_hhdm_offset();
    uint64_t top = pmm_highest_address();

    enable_nx();

    kernel_pml4 = vmm_new_address_space();
    if (kernel_pml4 == 0) {
        panic("vmm: no memory for even a single pml4. this is a bad start");
    }

    /* the direct map: every scrap of physical memory, readable and
     * writable but never executable. 2MiB pages, so the whole thing
     * costs a handful of tables rather than megabytes of them */
    if (!vmm_map_range(kernel_pml4, hhdm, 0, top, PTE_WRITE | nx)) {
        panic("vmm: ran out of memory building the direct map");
    }
    kprintf("  hhdm    %p .. %p  rw-  (%lu MiB, 2MiB pages)\n",
            (void *)hhdm, (void *)(hhdm + top), top / (1024 * 1024));

    /* and the kernel itself, one section at a time, each with only the
     * rights it actually needs. this is the whole point of the exercise */
    map_section(kernel_pml4, __kernel_start, __text_start, phys_base, virt_base,
                nx, "limine");           /* the request markers: read only */
    map_section(kernel_pml4, __text_start, __text_end, phys_base, virt_base,
                0, "text");              /* executable, NOT writable */
    map_section(kernel_pml4, __rodata_start, __rodata_end, phys_base, virt_base,
                nx, "rodata");           /* neither */
    map_section(kernel_pml4, __data_start, __data_end, phys_base, virt_base,
                PTE_WRITE | nx, "data"); /* writable, never executable */

    /* ---- now prove it works, while we can still complain ---- */
    require_maps_to(kernel_pml4, (uint64_t)kmain,
                    phys_base + ((uint64_t)kmain - virt_base), "kmain");
    require_maps_to(kernel_pml4, (uint64_t)__data_start,
                    phys_base + ((uint64_t)__data_start - virt_base), "data start");
    require_maps_to(kernel_pml4, hhdm, 0, "the base of the direct map");
    require_maps_to(kernel_pml4, hhdm + 0x1234000, 0x1234000, "a direct map page");

    /* the stack we are standing on, and some room below it for the
     * calls we are about to make. if this isnt mapped, loading cr3
     * would be the last thing this cpu ever did */
    uint64_t rsp = read_rsp();
    require_mapped(kernel_pml4, rsp, "the current stack");
    require_mapped(kernel_pml4, rsp - 0x4000, "room below the stack");

    /* and the page tables themselves, which we reach through the hhdm */
    require_mapped(kernel_pml4, (uint64_t)pmm_phys_to_virt(kernel_pml4),
                   "the pml4 itself");

    /* text must be executable and read only, data must be the reverse.
     * if these are backwards the machine still boots and the whole
     * exercise was pointless, so check */
    uint64_t text_flags = vmm_flags(kernel_pml4, (uint64_t)kmain);
    if (text_flags & PTE_WRITE) {
        panic("vmm: .text came out writable, W^X is not holding");
    }
    if (nx && (text_flags & PTE_NX)) {
        panic("vmm: .text came out non-executable, we would fault instantly");
    }
    uint64_t data_flags = vmm_flags(kernel_pml4, (uint64_t)__data_start);
    if (!(data_flags & PTE_WRITE)) {
        panic("vmm: .data came out read only, nothing would work");
    }
    if (nx && !(data_flags & PTE_NX)) {
        panic("vmm: .data came out executable, W^X is not holding");
    }

    enable_write_protect();

    /* the moment of truth. every instruction after this one is fetched
     * through tables we built ourselves */
    asm volatile ("mov %0, %%cr3" : : "r"(kernel_pml4) : "memory");

    kprintf("  -> cr3 is ours. %s, W^X on .text\n",
            nx ? "NX enabled" : "no NX available");
}

uint64_t vmm_nx(void) {
    return nx;
}

void vmm_flush_page(uint64_t virt) {
    asm volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_dump(uint64_t virt) {
    uint64_t phys = vmm_translate(kernel_pml4, virt);
    if (phys == VMM_NO_MAPPING) {
        kprintf("  %p is not mapped\n", (void *)virt);
        return;
    }
    uint64_t f = vmm_flags(kernel_pml4, virt);
    kprintf("  %p -> %p  %c%c%c%s\n", (void *)virt, (void *)phys,
            (f & PTE_PRESENT) ? 'p' : '-',
            (f & PTE_WRITE)   ? 'w' : '-',
            (f & PTE_NX)      ? '-' : 'x',
            (f & PTE_HUGE)    ? "  (2MiB page)" : "");
}

#endif /* TINYOS_HOSTED */
