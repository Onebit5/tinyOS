#include "cpu/idt.h"
#include "cpu/gdt.h"
#include <stdint.h>

/* the idt: 256 gates, every single one pointing at its stub from isr.asm.
 * unhandled vectors still go through the common path and get logged
 * instead of triple faulting, which is the whole point */

struct __attribute__((packed)) idt_entry {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;       /* interrupt stack table index, 0 = dont switch */
    uint8_t  flags;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t reserved;
};

struct __attribute__((packed)) idtr {
    uint16_t limit;
    uint64_t base;
};

static struct idt_entry idt[256];

/* built by isr.asm */
extern void *isr_stub_table[256];

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        uint64_t off = (uint64_t)isr_stub_table[i];
        idt[i] = (struct idt_entry){
            .off_low  = off & 0xffff,
            .selector = GDT_KERNEL_CODE,
            .ist      = 0,
            .flags    = 0x8e,   /* present, dpl 0, interrupt gate (ints off on entry) */
            .off_mid  = (off >> 16) & 0xffff,
            .off_high = (uint32_t)(off >> 32),
            .reserved = 0,
        };
    }

    struct idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt,
    };
    asm volatile ("lidt %0" : : "m"(idtr));
}
