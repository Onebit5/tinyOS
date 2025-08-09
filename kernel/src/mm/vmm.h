#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* virtual memory. four levels of page tables, which the manuals call
 * pml4 -> pdpt -> pd -> pt, and which everyone else calls "the reason
 * my kernel triple faults".
 *
 * up to now we were riding on the page tables limine built for us.
 * these are ours */

/* page table entry flags. the address lives in bits 12..51, the rest
 * is bookkeeping */
#define PTE_PRESENT (1ull << 0)
#define PTE_WRITE   (1ull << 1)
#define PTE_USER    (1ull << 2)
#define PTE_HUGE    (1ull << 7)     /* at pd level: a 2MiB page */
#define PTE_GLOBAL  (1ull << 8)
#define PTE_NX      (1ull << 63)    /* needs EFER.NXE, or its a fault */

#define PTE_ADDR_MASK 0x000ffffffffff000ull

#define PAGE_SIZE_2M (2ull * 1024 * 1024)

/* what vmm_translate says when nothing is mapped there. 0 would be
 * ambiguous, physical zero is a real (if useless) address */
#define VMM_NO_MAPPING UINT64_MAX

/* build our own address space, check it would actually work, and move
 * onto it. after this returns, cr3 is ours */
void vmm_init(void);

/* ---- address space surgery -----------------------------------------
 * these take the pml4 by physical address rather than assuming the
 * current one, which is what lets the host tests build a whole address
 * space over a malloc'd arena and inspect it without a cpu involved */

/* make a fresh empty pml4. returns its physical address, or 0 */
uint64_t vmm_new_address_space(void);

/* map size bytes. virt/phys/size get rounded out to page boundaries.
 * uses 2MiB pages where everything lines up, 4KiB otherwise */
bool vmm_map_range(uint64_t pml4, uint64_t virt, uint64_t phys,
                   uint64_t size, uint64_t flags);

/* walk the tables in software. returns the physical address that virt
 * would resolve to, or VMM_NO_MAPPING */
uint64_t vmm_translate(uint64_t pml4, uint64_t virt);

/* look up the flags on whatever maps virt (0 if nothing does) */
uint64_t vmm_flags(uint64_t pml4, uint64_t virt);

/* remove one 4KiB page from the map, splitting a 2MiB page into 512
 * small ones first if thats what it takes. this is how guard pages
 * get punched into the middle of the direct map */
bool vmm_unmap_page(uint64_t pml4, uint64_t virt);

/* the address space the kernel is actually running on */
uint64_t vmm_kernel_pml4(void);

/* PTE_NX, or 0 if this cpu cant honour it. always use this rather than
 * PTE_NX directly when building mappings at runtime: setting bit 63
 * while EFER.NXE is clear is a reserved-bit violation, so a hardcoded
 * PTE_NX would fault on every access on a machine without NX */
uint64_t vmm_nx(void);

/* drop one page from the tlb. changing a table entry is not enough --
 * the cpu caches translations and will happily keep using the old one */
void vmm_flush_page(uint64_t virt);

/* for the shell's `vmm` command */
void vmm_dump(uint64_t virt);

#endif
