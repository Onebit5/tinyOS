#ifndef ARCH_NONE_MACHINE_H
#define ARCH_NONE_MACHINE_H

#include <stdbool.h>

/* the three halves of a boot that are not portable. see arch/machine.h */
void machine_bring_up_early(void);
void machine_bring_up_late(void);
void machine_start_clock(void);

void machine_reset(void) __attribute__((noreturn));
bool machine_poweroff(void);
bool machine_key_pressed(void);

#endif
