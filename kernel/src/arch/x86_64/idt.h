#ifndef ARCH_X86_64_IDT_H
#define ARCH_X86_64_IDT_H

#include <stdint.h>

void idt_init(void);

/* the table is the same for every core -- the handlers are shared, and
 * only the stacks they land on differ, which is the tss's business. so a
 * core coming up just points at it */
void idt_load_here(void);

/* make one vector run on an IST stack instead of whatever stack was
 * current when it fired. index 1..7, or 0 for the normal behaviour */
void idt_set_ist(uint8_t vector, uint8_t ist);

#endif
