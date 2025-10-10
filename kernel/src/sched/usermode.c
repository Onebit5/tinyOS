#include "sched/usermode.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "cpu/gdt.h"
#include "drivers/input.h"
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
};

static void user_thread_start(void *arg) {
    struct user_start *u = arg;
    uint64_t entry = u->entry;
    uint64_t stack_top = u->stack_top;
    kfree(u);

    /* the scheduler loaded our address space and pointed the tss rsp0
     * and the syscall stack at our kernel stack when it switched us in,
     * so a trap from ring 3 lands somewhere we own. everything below
     * this line is one way */
    enter_usermode(entry, stack_top, GDT_USER_CODE3, GDT_USER_DATA3);
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

bool user_run(const char *path, bool background, const char **error) {
    reap_abandoned();

    struct ramdisk_file f;
    if (!ramdisk_open(path, &f)) {
        *error = USER_RUN_NO_SUCH_FILE;
        return false;
    }

    const char *why = NULL;
    if (!elf_is_loadable(f.data, f.size, &why)) {
        *error = why;
        return false;
    }

    /* its own memory. two programs can now link to the same addresses
     * and never meet, which is the whole point of this milestone */
    struct addrspace *space = addrspace_create(vmm_kernel_pml4());
    if (space == NULL) {
        *error = "no memory for an address space";
        return false;
    }

    struct elf_load_result loaded = elf_load(f.data, f.size, space->pml4);
    if (!loaded.ok) {
        addrspace_destroy(space);
        *error = loaded.error;
        return false;
    }

    /* a stack for ring 3: writable, never executable, mapped user */
    uint64_t stack_phys = pmm_alloc_pages(USER_STACK_PAGES);
    if (stack_phys == 0) {
        addrspace_destroy(space);
        *error = "no memory for a user stack";
        return false;
    }
    memset(pmm_phys_to_virt(stack_phys), 0, USER_STACK_PAGES * PAGE_SIZE);

    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (!vmm_map_range(space->pml4, stack_base, stack_phys,
                       USER_STACK_PAGES * PAGE_SIZE,
                       PTE_USER | PTE_WRITE | vmm_nx())) {
        pmm_free_pages(stack_phys, USER_STACK_PAGES);
        addrspace_destroy(space);
        *error = "could not map a user stack";
        return false;
    }

    struct user_start *start = kmalloc(sizeof *start);
    if (start == NULL) {
        addrspace_destroy(space);   /* which owns the stack by now */
        *error = "no memory";
        return false;
    }
    start->entry = loaded.entry;
    start->stack_top = USER_STACK_TOP & ~0xfull;   /* sysv wants 16-aligned */

    /* the process comes first, because it is what outlives the thread
     * and holds the exit code somebody will want to read */
    int pid = process_create(path, 0, pit_uptime_ms());
    if (pid == 0) {
        kfree(start);
        addrspace_destroy(space);
        *error = "the process table is full";
        return false;
    }

    struct thread *t = thread_create(path, user_thread_start, start);
    if (t == NULL) {
        int ignored;
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        process_collect(pid, &ignored);
        kfree(start);
        addrspace_destroy(space);
        *error = "no memory for a thread";
        return false;
    }
    t->space = space;
    t->pid   = pid;
    process_set_thread(pid, t->id);

    int id = t->id;
    kprintf("[kernel] %s is pid %d, ring 3 at %p%s\n",
            path, pid, (void *)loaded.entry, background ? " (background)" : "");

    if (background) {
        /* nobody is waiting, so nobody will collect it. leave the slot
         * for `ps` to show and let the next `run` sweep it up */
        return true;
    }

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
        /* peek rather than take: the program may be sitting on
         * SYS_READ waiting for the very keys we would swallow */
        if (input_peek() == KEY_CTRL_C) {
            (void)input_getchar();
            kprintf("^C\n");
            if (sched_kill(id) != SCHED_KILL_OK) {
                kprintf("[kernel] it would not stop\n");
                return true;
            }
        }
        sleep_ms(20);
    }

    /* collect it: take the code and free the slot, which is the whole
     * reason the process outlived the thread */
    int code = 0;
    if (process_collect(pid, &code)) {
        if (code == PROCESS_KILLED) {
            kprintf("[kernel] pid %d was killed\n", pid);
        } else if (code != 0) {
            kprintf("[kernel] pid %d exited with %d\n", pid, code);
        }
    }
    return true;
}
