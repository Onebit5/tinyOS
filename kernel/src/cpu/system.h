#ifndef CPU_SYSTEM_H
#define CPU_SYSTEM_H

/* machine-level odds and ends */

/* the full ceremony: farewell text, a pause, then the reset */
void reboot(void) __attribute__((noreturn));

/* ask the machine to switch itself off. these are the acpi shutdown
 * ports that qemu, bochs and virtualbox each watch for -- real hardware
 * wants me to parse acpi tables and find the right one, which is a
 * milestone of its own. if none of them take, I halt instead */
void system_poweroff(void) __attribute__((noreturn));

/* just pulse the 8042 reset line and hope. no printing, no waiting, no
 * interrupts required -- safe to call from inside a panic */
void system_reset(void) __attribute__((noreturn));

#endif
