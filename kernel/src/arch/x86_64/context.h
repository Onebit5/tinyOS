#ifndef ARCH_X86_64_CONTEXT_H
#define ARCH_X86_64_CONTEXT_H

#include <stdint.h>

#include "arch/inline.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/syscall.h"

/* a thread's hardware state on x86_64. reached through arch/context.h. */

/* switch.asm. saves the six callee-saved registers on the stack it is
 * leaving, swaps rsp, and returns onto the other one -- so "everything
 * about a parked thread is on its own stack" is literally true and the
 * saved thread is one word */
extern void switch_context(uint64_t *save_sp, uint64_t *load_sp);

/* usermode.asm. never returns */
void enter_usermode(uint64_t entry, uint64_t stack_top,
                    uint64_t cs, uint64_t ss,
                    uint64_t argc, uint64_t argv);

/* where the kernel lands when this thread traps.
 *
 * two registers because there are two doors. an interrupt from ring 3
 * makes the processor fetch rsp0 out of the task state segment; a
 * `syscall` instruction does no such thing and would keep running on the
 * caller's own stack, so the entry stub swaps to one it reads out of an
 * msr. forget either and ring 3 gets to choose where the kernel's stack
 * is, which is not a bug so much as an invitation */
ARCH_INLINE void context_set_kernel_stack(uint64_t top) {
    tss_set_rsp0(top);
    syscall_set_kernel_rsp(top);
}

/* the selectors live here rather than at the call site. ring 3 is
 * entered with a code and a stack selector whose low two bits are the
 * privilege being asked for -- which is an x86 sentence, and the process
 * code was speaking it until 0.2.20 */
__attribute__((noreturn))
ARCH_INLINE void context_enter_user(uint64_t entry, uint64_t stack_top,
                                    uint64_t argc, uint64_t argv) {
    enter_usermode(entry, stack_top, GDT_USER_CODE3, GDT_USER_DATA3,
                   argc, argv);
    __builtin_unreachable();
}

#endif
