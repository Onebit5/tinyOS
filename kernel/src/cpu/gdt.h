#ifndef CPU_GDT_H
#define CPU_GDT_H

/* selectors, offsets into the gdt */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10

void gdt_init(void);

#endif
