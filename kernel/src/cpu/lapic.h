#ifndef CPU_LAPIC_H
#define CPU_LAPIC_H

#include <stdint.h>
#include <stdbool.h>

/* the local apic: one per cpu, and the reason there can be more than
 * one cpu at all. the 8259 is a single chip shared by the machine; this
 * is a piece of the processor, which is why every core gets its own and
 * why an smp kernel cannot be built on the old one.
 *
 * on its own this is lateral -- the same interrupts arriving by a
 * better road -- and that is worth saying plainly. what it buys is
 * everything after it. */

/* the vector the timer arrives on. above the 8259's old range so the
 * two can coexist while we are still deciding which to trust */
#define LAPIC_TIMER_VECTOR   0x40
#define LAPIC_SPURIOUS_VECTOR 0xff

bool lapic_init(uint64_t phys_address);
bool lapic_available(void);

/* end of interrupt. every apic interrupt is acknowledged here rather
 * than at the pic, and forgetting is how you get exactly one of each */
void lapic_eoi(void);

uint32_t lapic_id(void);

/* the timer, running at `hz`. it is driven by the bus clock, whose
 * speed nobody will tell us, so it has to be measured against something
 * that already keeps time -- which is what the pit is still good for */
void lapic_timer_start(uint32_t hz, uint64_t ticks_per_second);

/* count the apic's own ticks over a known interval. `wait_ms` should
 * block for that long by some other means */
uint64_t lapic_calibrate(void (*wait_ms)(uint64_t), uint64_t ms);

#endif
