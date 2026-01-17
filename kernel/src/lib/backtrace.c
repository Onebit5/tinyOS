#include "lib/backtrace.h"
#include "lib/ksyms.h"
#include "lib/kprintf.h"
#include "mm/vmm.h"

#define MAX_FRAMES 24

/* what gcc leaves on the stack for every function, given I build with
 * -fno-omit-frame-pointer: the caller's rbp, then the return address */
struct frame {
    struct frame *caller_rbp;
    uint64_t      return_addr;
};

static void print_where(uint64_t addr) {
    /* the symbol table only covers the kernel. an address down in the
     * low half belongs to whatever program was running, and saying so
     * is more use than shrugging */
    if (addr < 0xffff800000000000ull) {
        kprintf("  %p  <in userspace, which I have no symbols for>\n",
                (void *)addr);
        return;
    }

    uint64_t off = 0;
    const char *name = ksym_lookup(addr, &off);
    if (name == NULL) {
        kprintf("  %p  <outside every function i know>\n", (void *)addr);
    } else {
        kprintf("  %p  %s+0x%lx\n", (void *)addr, name, off);
    }
}

/* I am almost certainly being called from a panic, so absolutely
 * nothing here may fault -- a page fault inside the backtrace printer
 * would bury the actual bug under a second one. every address gets
 * checked against the page tables before I dereference it */
static bool readable(const void *p) {
    uint64_t pml4 = vmm_kernel_pml4();
    if (pml4 == 0) {
        return false;       /* too early to be sure of anything */
    }
    uint64_t a = (uint64_t)p;
    if (vmm_translate(pml4, a) == VMM_NO_MAPPING) {
        return false;
    }
    /* a frame straddling a page boundary needs both pages */
    if (vmm_translate(pml4, a + sizeof(struct frame) - 1) == VMM_NO_MAPPING) {
        return false;
    }
    return true;
}

void kbacktrace(uint64_t rbp, uint64_t rip) {
    kprintf("call trace:\n");

    if (rip != 0) {
        print_where(rip);
    }

    if (rbp == 0) {
        asm volatile ("mov %%rbp, %0" : "=r"(rbp));
    }

    struct frame *f = (struct frame *)rbp;
    for (int depth = 0; depth < MAX_FRAMES; depth++) {
        if (f == NULL || ((uint64_t)f & 7) != 0 || !readable(f)) {
            break;
        }
        if (f->return_addr == 0) {
            break;      /* the zero I fabricate at the base of every thread */
        }

        print_where(f->return_addr);

        /* stacks grow down, so a caller's frame is always at a higher
         * address. anything else means the chain is garbage and
         * following it would loop or wander into nowhere */
        if (f->caller_rbp <= f) {
            kprintf("  (frame chain stops making sense here)\n");
            break;
        }
        f = f->caller_rbp;
    }
}
