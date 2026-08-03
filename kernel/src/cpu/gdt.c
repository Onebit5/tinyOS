#include "cpu/gdt.h"
#include <stdint.h>

/* segmentation is basically dead in long mode but the cpu still demands
 * a gdt, so here is the flattest one possible. limine gave us a perfectly
 * fine one but its living in bootloader memory we'll reclaim eventually,
 * plus we need our own once the tss shows up.
 *
 * not const: loading a tss later flips the busy bit in its descriptor,
 * so the table has to be writable */

static uint64_t gdt[] = {
    0,                      /* null descriptor, mandatory tribute */
    0x00af9a000000ffff,     /* 0x08 kernel code: present, exec, long mode */
    0x00af92000000ffff,     /* 0x10 kernel data: present, rw */
    0, 0,                   /* user code + data, reserved for the distant future */
    0, 0,                   /* tss descriptor, takes two slots, also future */
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

void gdt_init(void) {
    struct gdtr gdtr = {
        .limit = sizeof(gdt) - 1,
        .base  = (uint64_t)gdt,
    };

    /* lgdt, then reload every segment register. cs cant be mov'd into,
     * you have to far-return your way into it like its 1985 */
    asm volatile (
        "lgdt %0\n"
        "pushq %[cs]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %w[ds], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorl %%eax, %%eax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :
        : "m"(gdtr), [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");
}
