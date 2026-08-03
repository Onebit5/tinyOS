#include "cpu/interrupts.h"
#include "cpu/pic.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "drivers/console.h"
#include <stddef.h>

/* if isr.asm and the frame struct ever drift apart, fail the build
 * instead of debugging garbage register dumps at 2am */
_Static_assert(offsetof(struct interrupt_frame, vector) == 15 * 8,
               "interrupt_frame drifted from isr.asm push order");
_Static_assert(sizeof(struct interrupt_frame) == 22 * 8,
               "interrupt_frame drifted from isr.asm push order");

static const char *exception_names[32] = {
    "#DE divide error",
    "#DB debug",
    "NMI non-maskable interrupt",
    "#BP breakpoint",
    "#OF overflow",
    "#BR bound range exceeded",
    "#UD invalid opcode",
    "#NM device not available",
    "#DF double fault",
    "coprocessor segment overrun (how old is this cpu?)",
    "#TS invalid tss",
    "#NP segment not present",
    "#SS stack segment fault",
    "#GP general protection fault",
    "#PF page fault",
    "reserved (15)",
    "#MF x87 floating point",
    "#AC alignment check",
    "#MC machine check",
    "#XM simd floating point",
    "#VE virtualization",
    "#CP control protection",
    "reserved (22)", "reserved (23)", "reserved (24)", "reserved (25)",
    "reserved (26)", "reserved (27)",
    "#HV hypervisor injection",
    "#VC vmm communication",
    "#SX security",
    "reserved (31)",
};

static uint64_t read_cr2(void) {
    uint64_t v;
    asm volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void dump_frame(struct interrupt_frame *f) {
    kprintf("rax=%016lx rbx=%016lx rcx=%016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("rdx=%016lx rsi=%016lx rdi=%016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("rbp=%016lx r8 =%016lx r9 =%016lx\n", f->rbp, f->r8, f->r9);
    kprintf("r10=%016lx r11=%016lx r12=%016lx\n", f->r10, f->r11, f->r12);
    kprintf("r13=%016lx r14=%016lx r15=%016lx\n", f->r13, f->r14, f->r15);
    kprintf("rip=%016lx rsp=%016lx rflags=%08lx\n", f->rip, f->rsp, f->rflags);
    kprintf("cs=%02lx ss=%02lx err=%lx\n", f->cs, f->ss, f->error_code);
}

static void (*irq_handlers[16])(struct interrupt_frame *);

void irq_register(uint8_t irq, void (*handler)(struct interrupt_frame *)) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void interrupt_dispatch(struct interrupt_frame *f) {
    if (f->vector >= PIC_IRQ_BASE && f->vector < PIC_IRQ_BASE + 16) {
        uint8_t irq = f->vector - PIC_IRQ_BASE;
        if (pic_is_spurious(irq)) {
            return;
        }
        if (irq_handlers[irq]) {
            irq_handlers[irq](f);
        } else {
            kprintf("irq %u fired with nobody listening\n", irq);
        }
        pic_send_eoi(irq);
        return;
    }

    if (f->vector >= 32) {
        /* not an exception, not a pic line. shouldnt happen, dont die over it */
        kprintf("stray interrupt %lu, ignoring\n", f->vector);
        return;
    }

    /* cpu exception. print everything we know, then panic */
    console_set_colors(0xe64553, 0x101018);

    kprintf("\n\ncpu exception %lu: %s\n", f->vector, exception_names[f->vector]);

    if (f->vector == 14) {
        uint64_t cr2 = read_cr2();
        uint64_t e = f->error_code;
        kprintf("page fault at %p: %s during %s%s in %s mode\n",
                (void *)cr2,
                (e & 1) ? "protection violation" : "page not present",
                (e & 16) ? "instruction fetch" : ((e & 2) ? "write" : "read"),
                (e & 8) ? " (reserved bit set?!)" : "",
                (e & 4) ? "user" : "kernel");
    }

    dump_frame(f);

    panic("%s at rip=%016lx", exception_names[f->vector], f->rip);
}
