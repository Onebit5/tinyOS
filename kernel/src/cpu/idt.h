#ifndef CPU_IDT_H
#define CPU_IDT_H

#include <stdint.h>

void idt_init(void);

/* make one vector run on an IST stack instead of whatever stack was
 * current when it fired. index 1..7, or 0 for the normal behaviour */
void idt_set_ist(uint8_t vector, uint8_t ist);

#endif
