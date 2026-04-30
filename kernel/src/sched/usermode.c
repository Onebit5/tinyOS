#include "sched/usermode.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "cpu/gdt.h"
#include "drivers/input.h"
#include "drivers/tty.h"
#include "fs/elf.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/addrspace.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/process.h"
#include "drivers/pit.h"
#include "fs/pipe.h"

const char *const USER_RUN_NO_SUCH_FILE = "no such file";

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

    /* the scheduler loaded my address space and pointed the tss rsp0
     * and the syscall stack at my kernel stack when it switched me in,
     * so a trap from ring 3 lands somewhere I own. everything below
     * this line is one way */
    enter_usermode(entry, stack_top, GDT_USER_CODE3, GDT_USER_DATA3,
                   argc, argv);
}

/* collect anything that finished in the background and was never
 * waited for. a real system has the parent do this on its own schedule;
 * here the next `run` sweeps up, which keeps the table from filling
 * with the remains of programs nobody asked about */
static void reap_abandoned(void) {
    /* collecting one renumbers the walk under me, so finish and start
     * over rather than trying to carry on from where I was */
    bool collected_one = true;
    while (collected_one) {
        collected_one = false;
        for (size_t i = 0; ; i++) {
            const struct process *p = process_at(i);
            if (p == NULL) {
                break;
            }
            if (p->exited) {
                /* before the slot goes, since afterwards there is
                 * nothing left to ask which pipes it was holding */
                pipe_release_for(p->pid);
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
 * I am writing into a stack that belongs to an address space nobody
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

/* put a file where the console or the keyboard would have been.
 *
 * this is the whole of redirection. `> name` makes the file if it is
 * not there and empties it if it is, which is what everybody means by
 * it -- a short thing written over a long one should leave the short
 * thing, not the short thing followed by the tail of the long one.
 * `>>` keeps what is there and starts at the end. `<` just opens it. */
static bool redirect(int pid, int fd, const char *path, bool writing,
                     bool append, const char **error) {
    struct vfs_file f;
    int uid = process_uid(pid);

    if (!writing) {
        if (!vfs_open(path, &f) || f.is_dir) {
            *error = "no such file to read from";
            return false;
        }
        /* the same question `open` asks, asked in the same way. a file
         * a guest may not read is not one they may read by pointing an
         * arrow at it -- an arrow is not a way round anything */
        if (!vfs_may_read(&f, uid)) {
            *error = "that file is not yours to read";
            return false;
        }
    } else {
        if (uid != 0) {
            *error = "only the master may write files";
            return false;
        }
        if (!append) {
            /* truncate by removing it first. vfs_create opens what is
             * already there rather than emptying it, and this is the
             * one place that difference is visible to anybody */
            struct vfs_file existing;
            if (vfs_open(path, &existing)) {
                if (existing.is_dir) {
                    *error = "that is a directory";
                    return false;
                }
                vfs_unlink(path);
            }
        }
        if (!vfs_create(path, &f)) {
            *error = "cannot write there -- only the disk takes new files";
            return false;
        }
    }

    struct fd slot;
    memset(&slot, 0, sizeof slot);
    slot.size = f.size;
    slot.writing = writing;
    slot.pos = (writing && append) ? f.size : 0;

    if (f.kind == VFS_DISK) {
        slot.kind = FD_DISK;
        slot.cluster = f.cluster;
        slot.entry_sector = f.entry_sector;
        slot.entry_offset = f.entry_offset;
    } else {
        slot.kind = FD_MEMORY;
        slot.data = f.data;
    }

    return process_fd_install(pid, fd, &slot);
}

static bool install_pipe(int pid, int fd, struct pipe *p, bool writing) {
    struct fd slot;
    memset(&slot, 0, sizeof slot);
    slot.kind = FD_PIPE;
    slot.pipe = p;
    slot.writing = writing;
    return process_fd_install(pid, fd, &slot);
}

int user_spawn(const char *path, int argc, const char *const argv[],
               const char *cwd,
               int parent, int uid, bool announce,
               const struct spawn_io *io, const char **error) {
    reap_abandoned();

    /* a program off the ramdisk is already in memory and is used where
     * it lies; one off the disk has to be read in first, and `owned`
     * says which happened so it can be let go of afterwards */
    const void *image = NULL;
    uint64_t image_size = 0;
    bool owned = false;
    if (!vfs_slurp(path, &image, &image_size, &owned)) {
        *error = USER_RUN_NO_SUCH_FILE;
        return 0;
    }

    const char *why = NULL;
    if (!elf_is_loadable(image, image_size, &why)) {
        vfs_release(image, owned);
        *error = why;
        return 0;
    }

    /* its own memory. two programs can now link to the same addresses
     * and never meet, which is the whole point of this milestone */
    struct addrspace *space = addrspace_create(vmm_kernel_pml4());
    if (space == NULL) {
        vfs_release(image, owned);
        *error = "no memory for an address space";
        return 0;
    }

    struct elf_load_result loaded = elf_load(image, image_size, space->pml4);

    /* elf_load has copied every segment into the new address space, so
     * whatever I read the program out of is nobody's business now */
    vfs_release(image, owned);

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

    /* wherever whoever started it was standing. process_create already
     * copies the parent's, which is right for a program spawning
     * another -- but the shell is a kernel thread with no process entry
     * of its own, so it says where it is explicitly */
    if (pid != 0 && cwd != NULL) {
        process_set_cwd(pid, cwd);
    }
    /* whatever was asked for goes in before the thread exists, so the
     * program has never seen anything else in those slots */
    if (pid != 0 && io != NULL) {
        const char *why = NULL;
        bool ok = true;
        if (io->in != NULL) {
            ok = install_pipe(pid, FD_STDIN, io->in, false);
        } else if (io->in_path != NULL) {
            ok = redirect(pid, FD_STDIN, io->in_path, false, false, &why);
        }
        if (ok && io->out != NULL) {
            ok = install_pipe(pid, FD_STDOUT, io->out, true);
        } else if (ok && io->out_path != NULL) {
            ok = redirect(pid, FD_STDOUT, io->out_path, true, io->append, &why);
        }
        if (!ok) {
            int ignored;
            process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
            process_collect(pid, &ignored);
            kfree(start);
            addrspace_destroy(space);
            *error = (why != NULL) ? why : "could not redirect that";
            return 0;
        }
    }
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

bool user_pipeline(const struct stage *stages, int count, const char *cwd,
                   int uid, bool background, const char **error) {
    if (count < 1 || count > PIPELINE_MAX) {
        *error = "that is more commands than I can join up";
        return false;
    }

    /* one pipe between each neighbouring pair, so count-1 of them. each
     * is born holding one reader and one writer, and those are exactly
     * the two ends about to be handed out -- so the shell never holds a
     * reference of its own and never has to remember to drop one */
    struct pipe *pipes[PIPELINE_MAX - 1];
    for (int i = 0; i < count - 1; i++) {
        pipes[i] = pipe_create();
        if (pipes[i] == NULL) {
            for (int j = 0; j < i; j++) {
                pipe_close_read(pipes[j]);
                pipe_close_write(pipes[j]);
            }
            *error = "no memory for a pipe";
            return false;
        }
    }

    int pids[PIPELINE_MAX];
    int started = 0;

    for (int i = 0; i < count; i++) {
        struct spawn_io io;
        memset(&io, 0, sizeof io);
        io.in  = (i > 0)         ? pipes[i - 1] : NULL;
        io.out = (i < count - 1) ? pipes[i]     : NULL;
        io.in_path  = stages[i].in_path;
        io.out_path = stages[i].out_path;
        io.append   = stages[i].append;

        const char *why = NULL;
        int pid = user_spawn(stages[i].path, stages[i].argc,
                             (const char *const *)stages[i].argv,
                             cwd, 0, uid, false, &io, &why);
        pids[i] = pid;
        if (pid != 0) {
            started++;
            continue;
        }

        /* it never started, so it will never close the ends it was
         * going to be given -- and a pipe nobody closes is a neighbour
         * blocked forever. close them here instead, which the ones
         * either side see as end of file and a broken pipe, and they
         * finish by themselves */
        if (i == 0 && count == 1) {
            *error = why;
            return false;
        }
        kprintf("cannot run %s: %s\n", stages[i].path, why);
        pipe_close_read(io.in);
        pipe_close_write(io.out);

        if (i == 0) {
            *error = why;
        }
    }

    if (started == 0) {
        return false;
    }

    /* a pipeline with an & on the end is nobody's to wait for. the
     * terminal stays with the shell, and the next spawn sweeps up
     * whatever is left of these */
    if (background) {
        for (int i = 0; i < count; i++) {
            if (pids[i] != 0) {
                kprintf("[kernel] pid %d runs in the background\n", pids[i]);
            }
        }
        return true;
    }

    /* the last one gets the terminal. it is the only stage that could
     * sensibly want the keyboard -- everything earlier is reading from
     * whoever is in front of it -- and it is the one still running when
     * a person reaches for ctrl+c */
    int last = 0;
    for (int i = count - 1; i >= 0; i--) {
        if (pids[i] != 0) {
            last = pids[i];
            break;
        }
    }
    tty_set_foreground(last);

    /* wait for all of them, not just the last. the shell's prompt must
     * not come back while something in the middle is still printing --
     * and by id rather than by pointer, because the reaper may free a
     * thread the instant it dies and an id cannot dangle */
    for (int i = 0; i < count; i++) {
        if (pids[i] == 0) {
            continue;
        }
        const struct process *p = process_find(pids[i]);
        int id = (p != NULL) ? p->thread_id : 0;
        while (sched_thread_alive(id)) {
            sleep_ms(20);
        }
    }

    tty_set_foreground(TTY_SHELL);

    for (int i = 0; i < count; i++) {
        int code = 0;
        if (pids[i] != 0 && process_collect(pids[i], &code)) {
            if (code == PROCESS_KILLED) {
                /* the ordinary end of a pipeline whose reader stopped
                 * early, so it is only worth saying about the last one
                 * -- `yes | head` killing yes is the machinery working */
                if (i == count - 1) {
                    kprintf("[kernel] pid %d was killed\n", pids[i]);
                }
            }
        }
    }
    return true;
}

bool user_run(const char *path, int argc, const char *const argv[],
              const char *cwd,
              int uid, bool background, bool announce, const char **error) {
    int pid = user_spawn(path, argc, argv, cwd, 0, uid, announce,
                         NULL, error);
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

    /* the terminal is the program's now. I stop watching the keyboard
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
     * needs lifetime rules I do not have -- this polls every 20ms,
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
