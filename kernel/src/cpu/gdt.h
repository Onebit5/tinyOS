#ifndef CPU_GDT_H
#define CPU_GDT_H

#include <stdint.h>

/* selectors, offsets into the gdt */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18    /* reserved for usermode */
#define GDT_USER_CODE   0x20    /* reserved for usermode */
#define GDT_TSS         0x28    /* takes two slots, being 16 bytes wide */

void gdt_init(void);

/* fill in the tss descriptor. called by tss_init once it knows where
 * the tss actually lives */
void gdt_set_tss(uint64_t base, uint32_t limit);

#endif
