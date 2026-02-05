#ifndef CPU_SMP_H
#define CPU_SMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cpu/acpi.h"

/* waking the other cores.
 *
 * the firmware has been telling me how many processors this machine has
 * since 0.1.7, and I have been using exactly one of them. the others are
 * sitting in reset, and the only thing in the world that can bring one
 * out is an interrupt sent from another core's local apic -- which is
 * why this could not be attempted before there was a local apic to send
 * it from.
 *
 * a core that wakes does not resume anything. it *starts*, in real mode,
 * sixteen bits wide, at a page below one megabyte, knowing nothing. so
 * every one of them has to make the same climb into long mode that the
 * bootloader made, on a page of code left lying at a fixed address for
 * exactly that purpose.
 *
 * what they do once they arrive is, for now, nothing at all. they report
 * their own apic id -- which is proof they really executed my code on
 * that core, since it is the one thing they cannot get wrong or fake --
 * and then halt. giving them work is 0.2.2, and it cannot come first:
 * this kernel has thirty-nine places where it turns interrupts off and
 * calls that mutual exclusion, and every one of them is a lie the
 * moment a second core runs kernel code. 0.2.1 is about that. */

#define SMP_MAX_CPUS 32

/* the page a woken core starts executing at, in real mode. the startup
 * message carries a vector, and the vector is a page number: 8 means
 * 0x8000. it has to be below a megabyte because that is all real mode
 * can see */
#define SMP_TRAMPOLINE_VECTOR 0x08
#define SMP_TRAMPOLINE_ADDR   ((uint64_t)SMP_TRAMPOLINE_VECTOR * 0x1000)

/* the arguments left for it, past the code, at offsets the assembly
 * agrees with. keep these next to the ones in trampoline.asm */
#define SMP_ARG_CR3   0x0f00
#define SMP_ARG_STACK 0x0f08
#define SMP_ARG_ENTRY 0x0f10
#define SMP_ARG_CPU   0x0f18
#define SMP_ARG_EFER  0x0f20
#define SMP_ARG_LOUD  0x0f28

struct cpu {
    uint32_t index;             /* 0 is the one that booted the machine */
    uint32_t apic_id;           /* what the firmware said */

    /* what the core itself said once it was awake. it reads this out of
     * its own local apic, so a match is proof the code ran where it was
     * meant to and not twice on the same processor */
    volatile uint32_t reported_id;
    volatile bool     online;

    uint64_t stack_top;
    bool     bootstrap;
};

/* wake everything the firmware listed. safe to call on a machine with
 * one core, or no usable local apic, or firmware that will not say --
 * it does nothing and reports one cpu, which is what was true before */
bool smp_init(const struct acpi_info *info);

size_t smp_cpu_count(void);      /* how many the firmware described */
size_t smp_online_count(void);   /* how many actually answered */
const struct cpu *smp_cpu_at(size_t index);

/* which core is asking. by apic id, since nothing has been set up yet
 * that would make it cheaper, and there are never many to look through */
uint32_t smp_this_cpu(void);

#endif
