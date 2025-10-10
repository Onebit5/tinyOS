#include "cpu/syscall.h"
#include "cpu/msr.h"
#include <stdbool.h>
#include "cpu/gdt.h"
#include "drivers/input.h"
#include "drivers/pit.h"
#include "lib/kprintf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "sched/thread.h"

#define MSR_STAR   0xc0000081
#define MSR_LSTAR  0xc0000082
#define MSR_SFMASK 0xc0000084

#define EFER_SCE (1ull << 0)    /* without this, `syscall` is #UD */

#define RFLAGS_IF (1ull << 9)
#define RFLAGS_DF (1ull << 10)
#define RFLAGS_TF (1ull << 8)

uint64_t syscall_kernel_rsp;

/* implemented in syscall.asm */
extern void syscall_entry(void);

/* a pointer handed to us by ring 3 is a claim, not a fact. check the
 * whole span really is mapped before touching a byte of it -- a user
 * program should not be able to make the kernel fault by lying */
static bool user_range_ok(uint64_t addr, uint64_t len) {
    if (len == 0) {
        return true;
    }
    if (addr + len < addr) {
        return false;           /* wrapped, so it is a lie by construction */
    }
    /* nothing in userspace lives in the higher half, and letting a
     * pointer up there through would hand ring 3 the kernel */
    if (addr >= 0xffff800000000000ull || (addr + len) > 0xffff800000000000ull) {
        return false;
    }
    uint64_t pml4 = vmm_kernel_pml4();
    for (uint64_t p = addr & ~0xfffull; p < addr + len; p += PAGE_SIZE) {
        uint64_t flags = vmm_flags(pml4, p);
        if (!(flags & PTE_PRESENT) || !(flags & PTE_USER)) {
            return false;
        }
    }
    return true;
}

#define WRITE_MAX 4096

static int64_t sys_write(uint64_t ptr, uint64_t len) {
    /* clamp first, then check what we clamped to. a program asking to
     * write four exabytes gets a short write rather than a refusal,
     * which is the ordinary contract -- and the range actually checked
     * below is the one we actually touch, so an absurd length can
     * never widen what we are willing to read */
    if (len > WRITE_MAX) {
        len = WRITE_MAX;
    }
    if (!user_range_ok(ptr, len)) {
        return -1;
    }
    const char *s = (const char *)ptr;
    for (uint64_t i = 0; i < len; i++) {
        kprintf("%c", s[i]);
    }
    return (int64_t)len;
}

static int64_t sys_read(uint64_t ptr, uint64_t len) {
    if (!user_range_ok(ptr, len) || len == 0) {
        return -1;
    }
    char *buf = (char *)ptr;
    uint64_t n = 0;
    while (n < len) {
        int c = input_getchar_blocking();
        if (c < 0 || c > 0xff) {
            continue;           /* an arrow key is not a byte */
        }
        buf[n++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    return (int64_t)n;
}

/* the number is in rax, arguments in rdi rsi rdx rcx (the asm moved r10
 * there for us) and r8. returns into rax */
int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4) {
    (void)a2; (void)a3; (void)a4;

    switch (nr) {
    case SYS_EXIT:
        thread_exit((int)a0);   /* never returns */
    case SYS_WRITE:
        return sys_write(a0, a1);
    case SYS_READ:
        return sys_read(a0, a1);
    case SYS_UPTIME:
        return (int64_t)pit_uptime_ms();
    case SYS_YIELD:
        sched_yield();
        return 0;
    case SYS_SLEEP:
        sleep_ms(a0);
        return 0;
    default:
        kprintf("[kernel] thread asked for syscall %lu, which does not exist\n",
                nr);
        return -1;
    }
}

void syscall_init(void) {
    /* STAR[47:32] is the kernel selector pair syscall loads: cs from it
     * and ss from it+8. STAR[63:48] is the base sysret computes from,
     * cs = base+16 and ss = base+8, which is why the gdt puts user data
     * below user code */
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32)
                  | ((uint64_t)GDT_KERNEL_DATA << 48);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* flags to clear on entry. IF is the important one: it means we
     * arrive with interrupts off and can swap onto a kernel stack
     * without anything preempting us halfway. DF because the sysv abi
     * insists it be clear, TF so a user single-stepping cannot drag the
     * kernel along with it */
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}
