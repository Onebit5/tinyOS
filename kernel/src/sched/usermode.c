#include "sched/usermode.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "cpu/gdt.h"
#include "drivers/input.h"
#include "drivers/tty.h"
#include "fs/elf.h"
#include "fs/ramdisk.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/addrspace.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/process.h"
#include "drivers/pit.h"

const char *const USER_RUN_NO_SUCH_FILE = "no such file in the ramdisk";

/* what a user thread needs to know before it stops being a kernel one.
 * one per program, freed by the thread that reads it -- a static would
 * have been fine when only one program could exist, and is not now */
struct user_start {
    uint64_t entry;
    uint64_t stack_top;
    uint64_t argc;
    uint64_t argv;      /* a user address, inside that same stack */
};

static void user_thread_start(void *arg) {
    struct user_start *u = arg;
    uint64_t entry = u->entry;
    uint64_t stack_top = u->stack_top;
    uint64_t argc = u->argc;
    uint64_t argv = u->argv;
    kfree(u);

    /* the scheduler loaded our address space and pointed the tss rsp0
     * and the syscall stack at our kernel stack when it switched us in,
     * so a trap from ring 3 lands somewhere we own. everything below
     * this line is one way */
    enter_usermode(entry, stack_top, GDT_USER_CODE3, GDT_USER_DATA3,
                   argc, argv);
}

/* collect anything that finished in the background and was never
 * waited for. a real system has the parent do this on its own schedule;
 * here the next `run` sweeps up, which keeps the table from filling
 * with the remains of programs nobody asked about */
static void reap_abandoned(void) {
    /* collecting one renumbers the walk under us, so finish and start
     * over rather than trying to carry on from where we were */
    bool collected_one = true;
    while (collected_one) {
        collected_one = false;
        for (size_t i = 0; ; i++) {
            const struct process *p = process_at(i);
            if (p == NULL) {
                break;
            }
            if (p->exited) {
                process_collect(p->pid, NULL);
                collected_one = true;
                break;
            }
        }
    }
}

/* lay the arguments out on the program's own stack, top downwards:
 * first the strings, then an array of pointers to them, then the stack
 * pointer it will start on.
 *
 * we are writing into a stack that belongs to an address space nobody
 * has loaded yet, so every store goes through the direct map while
 * every *pointer* has to be the address the program will see. the two
 * run in lockstep, which is what user_addr() keeps straight. */
struct argblock {
    uint64_t stack_top;     /* where rsp starts, 16-aligned */
    uint64_t argv;          /* user address of the pointer array */
    uint64_t argc;
};

static bool build_args(uint64_t stack_phys, int argc, const char *const argv[],
                       struct argblock *out) {
    uint8_t *base_k = pmm_phys_to_virt(stack_phys);
    uint8_t *top_k  = base_k + USER_STACK_PAGES * PAGE_SIZE;

    /* the kernel address of a given user address inside this stack */
    #define user_addr(va) (top_k - (USER_STACK_TOP - (va)))

    uint64_t sp = USER_STACK_TOP;
    uint64_t str_va[MAX_ARGS];

    if (argc > MAX_ARGS) {
        argc = MAX_ARGS;
    }

    /* the strings themselves, backwards so argv[0] ends up lowest */
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = strlen(argv[i]) + 1;
        if (sp - len <= USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE + 256) {
            return false;       /* leave the program some stack to run on */
        }
        sp -= len;
        memcpy(user_addr(sp), argv[i], len);
        str_va[i] = sp;
    }

    /* then the array of pointers to them, aligned */
    sp &= ~15ull;
    sp -= (uint64_t)(argc + 1) * 8;
    uint64_t *arr = (uint64_t *)user_addr(sp);
    for (int i = 0; i < argc; i++) {
        arr[i] = str_va[i];
    }
    arr[argc] = 0;              /* the NULL every argv ends with */

    out->argv = sp;
    out->argc = (uint64_t)argc;
    out->stack_top = sp & ~15ull;   /* sysv wants rsp 16-aligned at entry */

    #undef user_addr
    return true;
}

int user_spawn(const char *path, int argc, const char *const argv[],
               int parent, int uid, bool announce, const char **error) {
    reap_abandoned();

    struct ramdisk_file f;
    if (!ramdisk_open(path, &f)) {
        *error = USER_RUN_NO_SUCH_FILE;
        return 0;
    }

    const char *why = NULL;
    if (!elf_is_loadable(f.data, f.size, &why)) {
        *error = why;
        return 0;
    }

    /* its own memory. two programs can now link to the same addresses
     * and never meet, which is the whole point of this milestone */
    struct addrspace *space = addrspace_create(vmm_kernel_pml4());
    if (space == NULL) {
        *error = "no memory for an address space";
        return 0;
    }

    struct elf_load_result loaded = elf_load(f.data, f.size, space->pml4);
    if (!loaded.ok) {
        addrspace_destroy(space);
        *error = loaded.error;
        return 0;
    }

    /* a stack for ring 3: writable, never executable, mapped user */
    uint64_t stack_phys = pmm_alloc_pages(USER_STACK_PAGES);
    if (stack_phys == 0) {
        addrspace_destroy(space);
        *error = "no memory for a user stack";
        return 0;
    }
    memset(pmm_phys_to_virt(stack_phys), 0, USER_STACK_PAGES * PAGE_SIZE);

    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (!vmm_map_range(space->pml4, stack_base, stack_phys,
                       USER_STACK_PAGES * PAGE_SIZE,
                       PTE_USER | PTE_WRITE | vmm_nx())) {
        pmm_free_pages(stack_phys, USER_STACK_PAGES);
        addrspace_destroy(space);
        *error = "could not map a user stack";
        return 0;
    }

    struct user_start *start = kmalloc(sizeof *start);
    if (start == NULL) {
        addrspace_destroy(space);   /* which owns the stack by now */
        *error = "no memory";
        return 0;
    }
    struct argblock args;
    if (!build_args(stack_phys, argc, argv, &args)) {
        kfree(start);
        addrspace_destroy(space);
        *error = "those arguments do not fit on a stack";
        return 0;
    }

    start->entry     = loaded.entry;
    start->stack_top = args.stack_top;
    start->argc      = args.argc;
    start->argv      = args.argv;

    /* the process comes first, because it is what outlives the thread
     * and holds the exit code somebody will want to read */
    /* a program cannot ask to be somebody else: it runs as whoever
     * started it, and only the shell decides what that is */
    int pid = process_create(path, parent, uid, announce, pit_uptime_ms());
    if (pid == 0) {
        kfree(start);
        addrspace_destroy(space);
        *error = "the process table is full";
        return 0;
    }

    struct thread *t = thread_create(path, user_thread_start, start);
    if (t == NULL) {
        int ignored;
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        process_collect(pid, &ignored);
        kfree(start);
        addrspace_destroy(space);
        *error = "no memory for a thread";
        return 0;
    }
    t->space = space;
    t->pid   = pid;
    process_set_thread(pid, t->id);

    if (announce) {
        kprintf("[kernel] %s is pid %d, ring 3 at %p\n",
                path, pid, (void *)loaded.entry);
    }
    return pid;
}

/* wait for a pid, however it ends. polling for the same reason the
 * foreground wait polls: the thread may be freed at any moment, and a
 * pid cannot dangle where a pointer would */
bool user_wait(int pid, int *code) {
    for (;;) {
        const struct process *p = process_find(pid);
        if (p == NULL) {
            return false;       /* gone, or somebody else collected it */
        }
        if (p->exited) {
            break;
        }
        sleep_ms(20);
    }
    return process_collect(pid, code);
}

bool user_run(const char *path, int argc, const char *const argv[],
              int uid, bool background, bool announce, const char **error) {
    int pid = user_spawn(path, argc, argv, 0, uid, announce, error);
    if (pid == 0) {
        return false;
    }

    if (background) {
        /* nobody is waiting, so nobody will collect it. the slot stays
         * for `ps` to show, and the next spawn sweeps it up */
        if (announce) {
            kprintf("[kernel] pid %d runs in the background\n", pid);
        }
        return true;
    }

    const struct process *p = process_find(pid);
    int id = (p != NULL) ? p->thread_id : 0;

    /* the terminal is the program's now. we stop watching the keyboard
     * entirely -- ctrl+c goes to it rather than being acted on for it,
     * and every other key is its to read. this is the difference
     * between a shell that waits and one that stands in the way */
    tty_set_foreground(pid);

    /* a foreground program is waited for, because that is what a shell
     * does. without it the prompt prints first and the program prints
     * over the top of it, leaving output with no prompt underneath.
     *
     * waiting by id rather than by pointer is deliberate: the reaper
     * may free the thread the moment it dies, and an id cannot dangle.
     * a real join would sleep on a waitq owned by the thread, which
     * needs lifetime rules we do not have -- this polls every 20ms,
     * which no human will notice. */
    while (sched_thread_alive(id)) {
        sleep_ms(20);
    }

    tty_set_foreground(TTY_SHELL);

    /* collect it: take the code and free the slot, which is the whole
     * reason the process outlived the thread */
    int code = 0;
    if (process_collect(pid, &code)) {
        /* being killed is always worth saying, because somebody asked
         * for it and deserves to know it happened. an exit code is
         * only interesting when the run was a demonstration */
        if (code == PROCESS_KILLED) {
            kprintf("[kernel] pid %d was killed\n", pid);
        } else if (announce && code != 0) {
            kprintf("[kernel] pid %d exited with %d\n", pid, code);
        }
    }
    return true;
}
