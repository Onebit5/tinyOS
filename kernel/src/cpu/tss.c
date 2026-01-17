#include "cpu/tss.h"
#include "cpu/gdt.h"
#include "mm/pmm.h"
#include "lib/panic.h"
#include "lib/string.h"

#define IST_STACK_PAGES 4       /* 16k, plenty for a handler that only reports */

/* the layout the cpu expects, and it is a strange one -- the reserved
 * holes are where the 32-bit fields used to live back when task
 * switching was a hardware feature */
struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];            /* rsp0..rsp2, one per privilege level */
    uint64_t reserved1;
    uint64_t ist[7];            /* ist1..ist7, indexed from 0 here */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

_Static_assert(sizeof(struct tss) == 104, "the tss is a fixed shape, check the packing");

static struct tss tss;

static uint64_t alloc_stack(const char *what) {
    uint64_t phys = pmm_alloc_pages(IST_STACK_PAGES);
    if (phys == 0) {
        panic("tss: no memory for the %s stack", what);
    }
    /* stacks grow down, so hand back the top */
    return (uint64_t)pmm_phys_to_virt(phys) + IST_STACK_PAGES * PAGE_SIZE;
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp[0] = rsp0;
}

void tss_init(void) {
    memset(&tss, 0, sizeof tss);

    tss.ist[IST_DOUBLE_FAULT - 1] = alloc_stack("double fault");

    /* where the cpu lands on a trap from ring 3. I have no ring 3 yet,
     * so this is one stack for the whole system -- when usermode
     * arrives it has to become per-thread, or two threads trapping at
     * once would land on the same stack and eat each other */
    tss_set_rsp0(alloc_stack("ring 0 entry"));

    /* an iomap base past the end of the segment means "no io bitmap",
     * which is how you say "ring 3 may not touch ports" */
    tss.iomap_base = sizeof tss;

    gdt_set_tss((uint64_t)&tss, sizeof(tss) - 1);

    asm volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS) : "memory");
}
