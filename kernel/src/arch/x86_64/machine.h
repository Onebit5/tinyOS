#ifndef ARCH_X86_64_MACHINE_H
#define ARCH_X86_64_MACHINE_H

#include <stdbool.h>

/* the pc, rather than the processor. reached through arch/machine.h.
 *
 * every one of these is a *pc* thing rather than an x86_64 thing -- the
 * 8042's reset line, the acpi ports three emulators happen to watch,
 * and a keyboard controller that has been at port 0x60 since 1981. an
 * x86_64 machine that was not a pc would need all three replaced and
 * not one instruction changed, which is the reason this header is not
 * arch/x86_64/cpu.h. */

void machine_reset(void) __attribute__((noreturn));
bool machine_poweroff(void);
bool machine_key_pressed(void);

#endif
