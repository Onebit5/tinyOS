#include "sched/usermode.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "cpu/gdt.h"
#include "fs/elf.h"
#include "fs/ramdisk.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "drivers/input.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* what a user thread needs to know before it stops being a kernel one.
 * it lives on the kernel side and is read once */
struct user_start {
    uint64_t entry;
    uint64_t stack_top;
};

static void user_thread_start(void *arg) {
    struct user_start *u = arg;
    uint64_t entry = u->entry;
    uint64_t stack_top = u->stack_top;

    /* the scheduler set the tss rsp0 and the syscall stack to this
     * thread's kernel stack when it switched us in, so a trap from ring
     * 3 has somewhere to land. everything below this line is one way */
    enter_usermode(entry, stack_top, GDT_USER_CODE3, GDT_USER_DATA3);
}

/* every program links to the same addresses and there is only one
 * address space, so a second one would map its segments straight over
 * the first's. separate address spaces per process is the fix and is a
 * milestone of its own; until then, one at a time, said plainly */
static struct thread *running;

const char *const USER_RUN_NO_SUCH_FILE = "no such file in the ramdisk";

bool user_run(const char *path, const char **error) {
    if (running != NULL && running->state != THREAD_DEAD) {
        *error = "a program is already running, and they would share an "
                 "address space. wait for it, or kill it";
        return false;
    }
    running = NULL;

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

    struct elf_load_result loaded = elf_load(f.data, f.size);
    if (!loaded.ok) {
        *error = loaded.error;
        return false;
    }

    /* a stack for ring 3. writable, never executable, and mapped user */
    uint64_t stack_phys = pmm_alloc_pages(USER_STACK_PAGES);
    if (stack_phys == 0) {
        *error = "no memory for a user stack";
        return false;
    }
    memset(pmm_phys_to_virt(stack_phys), 0, USER_STACK_PAGES * PAGE_SIZE);

    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (!vmm_map_range(vmm_kernel_pml4(), stack_base, stack_phys,
                       USER_STACK_PAGES * PAGE_SIZE,
                       PTE_USER | PTE_WRITE | vmm_nx())) {
        pmm_free_pages(stack_phys, USER_STACK_PAGES);
        *error = "could not map a user stack";
        return false;
    }
    for (uint64_t v = stack_base; v < USER_STACK_TOP; v += PAGE_SIZE) {
        vmm_flush_page(v);
    }

    /* the struct has to outlive this call, since the thread reads it
     * whenever the scheduler gets round to starting it. one program at
     * a time is all we support, so one static is all we need */
    static struct user_start start;
    start.entry = loaded.entry;
    /* sysv wants rsp 16-aligned at the entry point */
    start.stack_top = USER_STACK_TOP & ~0xfull;

    struct thread *t = thread_create(path, user_thread_start, &start);
    if (t == NULL) {
        pmm_free_pages(stack_phys, USER_STACK_PAGES);
        *error = "no memory for a thread";
        return false;
    }
    t->user_stack_phys = stack_phys;
    t->user_stack_pages = USER_STACK_PAGES;
    running = t;

    kprintf("[kernel] %s entered ring 3 at %p, thread %d\n",
            path, (void *)loaded.entry, t->id);

    /* and now wait for it, because `run` is a foreground command. the
     * alternative is what this used to do -- return at once, so the
     * shell printed its prompt and then the program printed over the
     * top of it, leaving you looking at output with no prompt under it
     * and no obvious way to tell whether anything was still running.
     *
     * waiting by id rather than by pointer is deliberate: the reaper
     * may free the thread the moment it dies, and an id cannot dangle.
     * a proper join would sleep on a waitq belonging to the thread,
     * which needs lifetime rules we do not have yet -- this polls
     * every 20ms instead, which no human will notice. */
    int id = t->id;
    while (sched_thread_alive(id)) {
        /* peek rather than take: a program may be waiting on SYS_READ
         * for the very keys we would otherwise swallow */
        if (input_peek() == KEY_CTRL_C) {
            (void)input_getchar();
            kprintf("^C\n");
            if (sched_kill(id) != SCHED_KILL_OK) {
                kprintf("[kernel] it is waiting on something and cannot be "
                        "stopped safely -- see `kill`\n");
                /* it is still alive and still holding the address
                 * space, so leave `running` set: a second program
                 * would map its segments straight over this one's */
                return true;
            }
        }
        sleep_ms(20);
    }

    running = NULL;
    return true;
}
