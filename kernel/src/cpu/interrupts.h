#ifndef CPU_INTERRUPTS_H
#define CPU_INTERRUPTS_H

#include <stdint.h>

/* what isr_common in isr.asm leaves on the stack, low address first.
 * if you touch the push order over there, touch this too. the static
 * asserts in interrupts.c will yell at you if you forget */

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* pushed by the cpu itself */
    uint64_t rip, cs, rflags, rsp, ss;
};

void interrupt_dispatch(struct interrupt_frame *frame);

/* hook a handler onto one of the 16 pic irq lines (0 = pit, 1 = keyboard...).
 * dispatch takes care of spurious irqs and the eoi, handlers just do their thing */
void irq_register(uint8_t irq, void (*handler)(struct interrupt_frame *));

#endif
