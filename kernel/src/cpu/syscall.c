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
#include "sched/process.h"
#include "sched/usermode.h"
#include "mm/addrspace.h"
#include "fs/ramdisk.h"
#include "drivers/tty.h"
#include "lib/string.h"

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

static int caller_pid(void);

/* whose page tables decide whether a user pointer is real.
 *
 * it must be the *caller's*, not the kernel's. every program has had an
 * address space of its own since 0.1.0, and the kernel's tables have no
 * mapping for a program's memory at all -- so checking there says no to
 * every pointer that was ever going to be valid, and a program prints
 * nothing for no visible reason. we are running on the caller's cr3 at
 * this moment, so this is also what the cpu would use if we simply
 * dereferenced the thing */
static uint64_t caller_pml4(void) {
    struct thread *me = sched_current();
    if (me != NULL && me->space != NULL) {
        return me->space->pml4;
    }
    return vmm_kernel_pml4();
}

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
    uint64_t pml4 = caller_pml4();
    for (uint64_t p = addr & ~0xfffull; p < addr + len; p += PAGE_SIZE) {
        uint64_t flags = vmm_flags(pml4, p);
        if (!(flags & PTE_PRESENT) || !(flags & PTE_USER)) {
            /* say so. a refusal returns -1 to a program that will
             * probably ignore it, and the result is a program that
             * prints nothing for no reason anyone can see -- which is
             * exactly how the 0.1.0 version of this bug stayed hidden */
            kprintf("[kernel] refused a pointer from pid %d: %p is not "
                    "this process's memory\n", caller_pid(), (void *)addr);
            return false;
        }
    }
    return true;
}

#define WRITE_MAX 4096

/* which process is asking. everything touching per-process state goes
 * through this rather than assuming */
static int caller_pid(void) {
    struct thread *me = sched_current();
    return (me != NULL) ? me->pid : 0;
}

/* copy a path out of ring 3 into somewhere we can trust it. paths are
 * short by definition, so a fixed buffer is honest rather than lazy */
static bool copy_path(uint64_t ptr, uint64_t len, char *out, size_t max) {
    if (len == 0 || len >= max || !user_range_ok(ptr, len)) {
        return false;
    }
    const char *src = (const char *)ptr;
    for (uint64_t i = 0; i < len; i++) {
        out[i] = src[i];
    }
    out[len] = '\0';
    return true;
}

static int64_t sys_write_console(uint64_t ptr, uint64_t len);

static int64_t sys_write(uint64_t fd, uint64_t ptr, uint64_t len) {
    /* nothing here is writable but the console: a descriptor onto the
     * ramdisk is a bookmark into read-only memory */
    if (fd != FD_STDOUT && fd != FD_STDERR) {
        return -1;
    }
    return sys_write_console(ptr, len);
}

static int64_t sys_write_console(uint64_t ptr, uint64_t len) {
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

/* the terminal does the echoing and the line editing, because a program
 * in ring 3 cannot -- the keys never pass through it */
static int64_t sys_read_stdin(uint64_t ptr, uint64_t len) {
    return tty_read_line(caller_pid(), (char *)ptr, len);
}

static int64_t sys_read(uint64_t fd, uint64_t ptr, uint64_t len) {
    if (len == 0 || !user_range_ok(ptr, len)) {
        return -1;
    }
    if (fd == FD_STDIN) {
        return sys_read_stdin(ptr, len);
    }

    /* a file. the bytes are already in memory -- the descriptor only
     * says how far through them we had got */
    const void *data = NULL;
    uint64_t left = 0;
    if (!process_fd_peek(caller_pid(), (int)fd, &data, &left)) {
        return -1;
    }
    if (left < len) {
        len = left;             /* a short read at the end, as usual */
    }
    memcpy((void *)ptr, data, len);
    process_fd_advance(caller_pid(), (int)fd, len);
    return (int64_t)len;
}

static int64_t sys_open(uint64_t ptr, uint64_t len) {
    char path[64];
    if (!copy_path(ptr, len, path, sizeof path)) {
        return -1;
    }
    struct ramdisk_file f;
    if (!ramdisk_open(path, &f)) {
        return -1;
    }
    return process_fd_open(caller_pid(), f.data, f.size);
}

static int64_t sys_spawn(uint64_t ptr, uint64_t len) {
    char path[64];
    if (!copy_path(ptr, len, path, sizeof path)) {
        return -1;
    }
    const char *why = NULL;
    int pid = user_spawn(path, caller_pid(), &why);
    return (pid == 0) ? -1 : pid;
}

/* block until a child ends, then hand back how it went. a program may
 * only wait for something it started -- otherwise one process could
 * collect another's child, and the exit code would go to the wrong
 * place entirely */
static int64_t sys_wait(uint64_t pid, uint64_t code_ptr) {
    const struct process *p = process_find((int)pid);
    if (p == NULL || p->parent != caller_pid()) {
        return -1;
    }
    if (code_ptr != 0 && !user_range_ok(code_ptr, sizeof(int))) {
        return -1;
    }

    int code = 0;
    if (!user_wait((int)pid, &code)) {
        return -1;
    }
    if (code_ptr != 0) {
        *(int *)code_ptr = code;
    }
    return (int64_t)pid;
}

/* the number is in rax, arguments in rdi rsi rdx rcx (the asm moved r10
 * there for us) and r8. returns into rax */
int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4) {
    (void)a3; (void)a4;

    switch (nr) {
    case SYS_EXIT:
        thread_exit((int)a0);   /* never returns */
    case SYS_WRITE:
        return sys_write(a0, a1, a2);
    case SYS_READ:
        return sys_read(a0, a1, a2);
    case SYS_OPEN:
        return sys_open(a0, a1);
    case SYS_CLOSE:
        return process_fd_close(caller_pid(), (int)a0) ? 0 : -1;
    case SYS_GETPID:
        return caller_pid();
    case SYS_SPAWN:
        return sys_spawn(a0, a1);
    case SYS_WAIT:
        return sys_wait(a0, a1);
    case SYS_UPTIME:
        return (int64_t)pit_uptime_ms();
    case SYS_YIELD:
        sched_yield();
        return 0;
    case SYS_SLEEP:
        sleep_ms(a0);
        /* an interrupt wakes a sleeper early, and it should be able to
         * tell that is what happened rather than think time passed */
        return process_take_interrupt(caller_pid()) ? -1 : 0;
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
