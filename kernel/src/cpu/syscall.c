#include "cpu/syscall.h"
#include "cpu/smp.h"
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
#include "fs/vfs.h"
#include "fs/path.h"
#include "drivers/tty.h"
#include "sched/auth.h"
#include "lib/string.h"

#define MSR_STAR   0xc0000081
#define MSR_LSTAR  0xc0000082
#define MSR_SFMASK 0xc0000084

#define EFER_SCE (1ull << 0)    /* without this, `syscall` is #UD */

#define RFLAGS_IF (1ull << 9)
#define RFLAGS_DF (1ull << 10)
#define RFLAGS_TF (1ull << 8)

static uint64_t call_counts[SYSCALL_COUNT];

static const char *const call_names[SYSCALL_COUNT] = {
    "exit", "write", "read", "uptime", "yield", "sleep",
    "open", "close", "getpid", "spawn", "wait", "readdir", "getuid",
    "create", "chdir", "getcwd", "mkdir", "rmdir",
    "unlink", "rename", "stat",
};

uint64_t syscall_times_called(unsigned nr) {
    return (nr < SYSCALL_COUNT) ? call_counts[nr] : 0;
}

const char *syscall_name(unsigned nr) {
    return (nr < SYSCALL_COUNT) ? call_names[nr] : "?";
}

/* implemented in syscall.asm */
extern void syscall_entry(void);

static int caller_pid(void);

/* whose page tables decide whether a user pointer is real.
 *
 * it must be the *caller's*, not the kernel's. every program has had an
 * address space of its own since 0.1.0, and the kernel's tables have no
 * mapping for a program's memory at all -- so checking there says no to
 * every pointer that was ever going to be valid, and a program prints
 * nothing for no visible reason. I am running on the caller's cr3 at
 * this moment, so this is also what the cpu would use if I simply
 * dereferenced the thing */
static uint64_t caller_pml4(void) {
    struct thread *me = sched_current();
    if (me != NULL && me->space != NULL) {
        return me->space->pml4;
    }
    return vmm_kernel_pml4();
}

/* a pointer handed to me by ring 3 is a claim, not a fact. check the
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

/* copy a path out of ring 3 into somewhere I can trust it. paths are
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

/* a path, as the caller wrote it, flattened against where the caller is
 * standing. every name that crosses this boundary goes through here --
 * relative or absolute, `..` and all -- so that no filesystem below ever
 * sees a path that means something different depending on who asked */
static bool copy_path_resolved(uint64_t ptr, uint64_t len,
                               char *out, size_t size) {
    char raw[PATH_MAX];
    if (!copy_path(ptr, len, raw, sizeof raw)) {
        return false;
    }
    return path_resolve(process_cwd(caller_pid()), raw, out, size);
}

static int64_t sys_write_console(uint64_t ptr, uint64_t len);

static int64_t sys_write(uint64_t fd, uint64_t ptr, uint64_t len) {
    if (fd == FD_STDOUT || fd == FD_STDERR) {
        return sys_write_console(ptr, len);
    }

    /* a descriptor onto the ramdisk is a bookmark into read-only
     * memory, so it stays unwritable. one onto the disk is not */
    struct fd_disk d;
    if (!process_fd_disk(caller_pid(), (int)fd, &d)) {
        return -1;
    }
    if (len > WRITE_MAX) {
        len = WRITE_MAX;
    }
    if (!user_range_ok(ptr, len)) {
        return -1;
    }

    /* the descriptor remembered where this file's directory record is,
     * which is what lets the new size be written back to the right place
     * without looking the path up all over again */
    struct vfs_file f;
    memset(&f, 0, sizeof f);
    f.kind = VFS_DISK;
    f.cluster = d.cluster;
    f.size = d.size;
    f.entry_sector = d.entry_sector;
    f.entry_offset = d.entry_offset;

    int64_t n = vfs_write(&f, d.pos, (const void *)ptr, len);
    if (n > 0) {
        process_fd_grew(caller_pid(), (int)fd, f.cluster, f.size);
        process_fd_advance(caller_pid(), (int)fd, (uint64_t)n);
    }
    return n;
}

static int64_t sys_write_console(uint64_t ptr, uint64_t len) {
    /* clamp first, then check what I clamped to. a program asking to
     * write four exabytes gets a short write rather than a refusal,
     * which is the ordinary contract -- and the range actually checked
     * below is the one I actually touch, so an absurd length can
     * never widen what I am willing to read */
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

    /* a file on the disk. the bytes are not in memory, so the
     * descriptor's position and the file's first cluster are enough to
     * go and get them */
    struct fd_disk d;
    if (process_fd_disk(caller_pid(), (int)fd, &d)) {
        if (d.remaining == 0) {
            return 0;           /* the end, which is not an error */
        }
        if (len > d.remaining) {
            len = d.remaining;
        }
        struct vfs_file f;
        memset(&f, 0, sizeof f);
        f.kind = VFS_DISK;
        f.cluster = d.cluster;
        f.size = d.size;

        int64_t n = vfs_read(&f, d.pos, (void *)ptr, len);
        if (n > 0) {
            process_fd_advance(caller_pid(), (int)fd, (uint64_t)n);
        }
        return n;
    }

    /* a file in the ramdisk. the bytes are already in memory -- the
     * descriptor only says how far through them I had got */
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
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }

    /* which filesystem a name means is the vfs's problem now, not this
     * one's. there used to be two branches here and a prefix test */
    struct vfs_file f;
    if (!vfs_open(path, &f) || f.is_dir) {
        return -1;
    }

    /* the boundary, in one line. the mode came off the file and the uid
     * off the process, and neither is anything ring 3 can reach in and
     * change */
    if (!vfs_may_read(&f, process_uid(caller_pid()))) {
        kprintf("[kernel] pid %d (uid %d) may not read %s\n",
                caller_pid(), process_uid(caller_pid()), path);
        return -1;
    }

    if (f.kind == VFS_DISK) {
        return process_fd_open_disk(caller_pid(), f.cluster, f.size,
                                    f.entry_sector, f.entry_offset);
    }
    return process_fd_open(caller_pid(), f.data, f.size);
}

/* make a file on the disk, or open one that is there, for writing. the
 * ramdisk cannot do this and says so -- it is a tar file in read-only
 * memory, and there is nowhere for a new file to go */
static int64_t sys_create(uint64_t ptr, uint64_t len) {
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not write here\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }

    struct vfs_file f;
    if (!vfs_create(path, &f)) {
        return -1;
    }
    return process_fd_open_disk(caller_pid(), f.cluster, f.size,
                                f.entry_sector, f.entry_offset);
}

/* ---- where a process is standing ---- */

static int64_t sys_chdir(uint64_t ptr, uint64_t len) {
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }

    /* it has to exist and it has to be a directory -- otherwise every
     * name used afterwards would resolve against somewhere that is not
     * there, and fail for a reason nowhere near the mistake */
    struct vfs_file f;
    if (!path_is_root(path)) {
        if (!vfs_open(path, &f) || !f.is_dir) {
            return -1;
        }
    }

    process_set_cwd(caller_pid(), path);
    return 0;
}

static int64_t sys_getcwd(uint64_t ptr, uint64_t len) {
    if (len == 0 || !user_range_ok(ptr, len)) {
        return -1;
    }
    const char *here = process_cwd(caller_pid());
    uint64_t n = strlen(here);
    if (n >= len) {
        n = len - 1;
    }
    memcpy((void *)ptr, here, n);
    ((char *)ptr)[n] = '\0';
    return (int64_t)n;
}

static int64_t sys_mkdir(uint64_t ptr, uint64_t len) {
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not make directories\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_mkdir(path) ? 0 : -1;
}

static int64_t sys_rmdir(uint64_t ptr, uint64_t len) {
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not remove directories\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_rmdir(path) ? 0 : -1;
}

static int64_t sys_unlink(uint64_t ptr, uint64_t len) {
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not remove files\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_unlink(path) ? 0 : -1;
}

static int64_t sys_rename(uint64_t from_ptr, uint64_t from_len,
                          uint64_t to_ptr, uint64_t to_len) {
    char from[PATH_MAX], to[PATH_MAX];
    if (!copy_path_resolved(from_ptr, from_len, from, sizeof from) ||
        !copy_path_resolved(to_ptr, to_len, to, sizeof to)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not rename files\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_rename(from, to) ? 0 : -1;
}

/* what a file is, without opening it. `ls -l` wants a size and a date
 * for every name in a directory, and opening each one to find out would
 * mean a descriptor per file for information that is in the directory
 * entry the readdir already read */
static int64_t sys_stat(uint64_t ptr, uint64_t len, uint64_t out_ptr) {
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (!user_range_ok(out_ptr, sizeof(struct user_stat))) {
        return -1;
    }

    struct vfs_file f;
    if (!vfs_open(path, &f)) {
        return -1;
    }
    if (!vfs_may_read(&f, process_uid(caller_pid()))) {
        return -1;
    }

    struct user_stat st;
    memset(&st, 0, sizeof st);
    st.size = f.size;
    st.mode = f.mode;
    st.is_dir = f.is_dir ? 1 : 0;
    st.year = f.written.year;
    st.month = f.written.month;
    st.day = f.written.day;
    st.hour = f.written.hour;
    st.minute = f.written.minute;
    st.second = f.written.second;

    memcpy((void *)out_ptr, &st, sizeof st);
    return 0;
}

/* the nth file in the ramdisk, by name. this is the whole of readdir:
 * there are no directories to descend into, so an index and a name is
 * the entire interface. `ls` needed exactly this and nothing else --
 * it was the only thing keeping it inside the kernel */
static int64_t sys_readdir(uint64_t index, uint64_t ptr, uint64_t len,
                           uint64_t path_ptr, uint64_t path_len) {
    if (len == 0 || !user_range_ok(ptr, len)) {
        return -1;
    }

    /* no path means the root, which is where anyone looking around
     * would start. there is only one namespace now, so this needs no
     * idea of which filesystem it is walking */
    char path[PATH_MAX] = "/";
    if (path_len > 0
        && !copy_path_resolved(path_ptr, path_len, path, sizeof path)) {
        return -1;
    }
    if (path_len == 0) {
        /* no path means "where I am", which is now a real answer */
        const char *here = process_cwd(caller_pid());
        size_t i = 0;
        while (here[i] != '\0' && i < sizeof path - 1) {
            path[i] = here[i];
            i++;
        }
        path[i] = '\0';
    }

    struct vfs_file f;
    if (!vfs_readdir(path, (size_t)index, &f)) {
        return -1;      /* past the end */
    }

    /* a directory comes back with a trailing slash, which is how
     * everyone has said "this one can be descended into" since long
     * before any of this. no protocol needed */
    uint64_t n = strlen(f.name);
    bool slash = f.is_dir && (n == 0 || f.name[n - 1] != '/');
    if (slash) {
        n++;
    }
    if (n >= len) {
        n = len - 1;
    }
    memcpy((void *)ptr, f.name, slash ? n - 1 : n);
    if (slash && n > 0) {
        ((char *)ptr)[n - 1] = '/';
    }
    ((char *)ptr)[n] = '\0';
    return (int64_t)n;
}

static int64_t sys_spawn(uint64_t ptr, uint64_t len) {
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    /* a spawned program gets its own path as argv[0], the way a shell
     * would give it. richer arguments want a syscall that can carry
     * them, which is not this one.
     *
     * it also inherits my uid rather than choosing one: a program that
     * could pick its own user would make the whole idea decorative */
    const char *why = NULL;
    const char *argv[1] = { path };
    int pid = user_spawn(path, 1, argv, process_cwd(caller_pid()), caller_pid(),
                         process_uid(caller_pid()), false, &why);
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
 * there for me) and r8. returns into rax */
int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4) {
    if (nr < SYSCALL_COUNT) {
        call_counts[nr]++;
    }

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
    case SYS_READDIR:
        return sys_readdir(a0, a1, a2, a3, a4);
    case SYS_CREATE:
        return sys_create(a0, a1);
    case SYS_CHDIR:
        return sys_chdir(a0, a1);
    case SYS_GETCWD:
        return sys_getcwd(a0, a1);
    case SYS_MKDIR:
        return sys_mkdir(a0, a1);
    case SYS_RMDIR:
        return sys_rmdir(a0, a1);
    case SYS_UNLINK:
        return sys_unlink(a0, a1);
    case SYS_RENAME:
        return sys_rename(a0, a1, a2, a3);
    case SYS_STAT:
        return sys_stat(a0, a1, a2);
    case SYS_GETUID:
        return process_uid(caller_pid());
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

/* one pair of words per core, reached through gs by the entry stub.
 * these cannot be globals any more: two cores in a syscall at the same
 * instant would trample each other's stacks, and the scheduler on one
 * core would be rewriting the kernel stack the other is about to use */
struct syscall_percpu {
    uint64_t kernel_rsp;    /* gs:0 -- keep this first */
    uint64_t scratch_rsp;   /* gs:8 */
};

static struct syscall_percpu syscall_cpu[SMP_MAX_CPUS];

void syscall_set_kernel_rsp(uint64_t rsp) {
    syscall_cpu[smp_this_cpu()].kernel_rsp = rsp;
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

    /* flags to clear on entry. IF is the important one: it means I
     * arrive with interrupts off and can swap onto a kernel stack
     * without anything preempting me halfway. DF because the sysv abi
     * insists it be clear, TF so a user single-stepping cannot drag the
     * kernel along with it */
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* gs names this core, in both rings, for as long as the machine
     * runs. both halves are set to the same thing so that a stray
     * swapgs -- from anywhere, ever -- is a no-op rather than a fault
     * three instructions into the next system call.
     *
     * every one of the registers set in this function is per core, which
     * is the whole reason it runs on each of them rather than once */
    uint64_t mine = (uint64_t)&syscall_cpu[smp_this_cpu()];
    wrmsr(MSR_GS_BASE, mine);
    wrmsr(MSR_KERNEL_GS_BASE, mine);
}
