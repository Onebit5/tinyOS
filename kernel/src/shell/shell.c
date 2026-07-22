#include "shell/shell.h"
#include "drivers/input.h"
#include "drivers/console.h"
#include "drivers/tty.h"
#include "drivers/pit.h"
#include "cpu/system.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "mm/vmm.h"
#include "lib/backtrace.h"
#include "drivers/rtc.h"
#include "cpu/cpuinfo.h"
#include "fs/ramdisk.h"
#include "fs/disk.h"
#include "fs/vfs.h"
#include "fs/path.h"
#include "drivers/ahci.h"
#include "cpu/smp.h"
#include "sched/usermode.h"
#include "fs/pipe.h"
#include "sched/auth.h"
#include "cpu/syscall.h"
#include "cpu/interrupts.h"
#include "drivers/pci.h"
#include "lib/ksyms.h"
#include "version.h"
#include "sched/sched.h"
#include "sched/spinlock.h"
#include "sched/thread.h"
#include "sched/process.h"
#include <stdint.h>
#include <stdbool.h>

#define LINE_MAX 128
#define ARGV_MAX 8
#define HISTORY_MAX 16

#define COLOR_PROMPT 0x7b8ce0   /* velvet blue */
#define COLOR_TEXT   0xc8c8d0
#define COLOR_WARN   0xe6c245

struct command {
    const char *name;
    const char *help;
    void (*fn)(int argc, char **argv);
    bool takes_file;    /* tab should offer ramdisk names after it */

    /* the shape of the command line, for `help <name>`. NULL when the
     * name alone is the whole of it */
    const char *usage;
};

static const struct command commands[];    /* defined below, after the handlers */
static void run_argv(int argc, char **argv);

/* where the shell is standing. declared here because looking a command
 * up has to read it, and that happens before the editing code that owns
 * the rest of the shell's state */
#define JOBS_MAX 8

enum job_state { JOB_FREE = 0, JOB_RUNNING, JOB_STOPPED };

struct shell_job {
    int             number;         /* what you type after fg */
    enum job_state  state;
    struct job      j;
    char            line[LINE_MAX];
};

/* ---- a session ------------------------------------------------------
 *
 * there are four consoles now, and a console with a shell on it is a
 * session: somebody logged in, standing somewhere, with their own
 * history and their own jobs. all of that used to be file-static, which
 * was correct while there was one of them and became a bug the moment
 * there were four -- four shells sharing one working directory is one
 * shell with four windows onto it.
 *
 * so it is a struct, one per console, and every shell function reaches
 * it through me(). which one is *not* passed in: it is whichever
 * console the calling thread is on, because that is always the right
 * answer and an argument would only be a chance to pass the wrong one */

#define HISTORY_SESSION_MAX 16

struct session {
    char cwd[PATH_MAX];

    int  uid;
    char user[AUTH_NAME_MAX];

    char history[HISTORY_SESSION_MAX][LINE_MAX];
    int  hist_count;

    struct shell_job jobs[JOBS_MAX];
    int  next_job_number;
    int  current_job;

    /* the line as it was typed, kept so a job can be named later */
    char typed_line[LINE_MAX];
};

static struct session sessions[VCONSOLE_COUNT];

static struct session *me(void) {
    unsigned n = tty_my_console();
    return &sessions[n < VCONSOLE_COUNT ? n : 0];
}

#define shell_cwd       (me()->cwd)
#define current_uid     (me()->uid)
#define current_user    (me()->user)
#define history         (me()->history)
#define hist_count      (me()->hist_count)
#define jobs            (me()->jobs)
#define next_job_number (me()->next_job_number)
#define current_job     (me()->current_job)
#define typed_line      (me()->typed_line)

/* where a bare command name is looked for, in order.
 *
 * the working directory is deliberately not on this list. a name typed
 * on its own should mean the same thing wherever you happen to be
 * standing, and a program left lying in a directory should not quietly
 * become a command there. say `./name` if that is what you mean -- and
 * anything with a slash in it is taken as a path and looked for exactly
 * where it says. */
static const char *const command_path[] = { "/bin", "/boot/bin", NULL };

/* turn a typed word into a program to run. `out` comes back holding an
 * absolute path. false means there is no such program anywhere I look */
static bool find_program(const char *word, char *out, size_t size) {
    bool has_slash = false;
    for (const char *p = word; *p != '\0'; p++) {
        if (*p == '/') {
            has_slash = true;
            break;
        }
    }

    struct vfs_file f;

    /* a path is a path: taken literally, read from where I am standing,
     * and not searched for anywhere else */
    if (has_slash) {
        if (!path_resolve(shell_cwd, word, out, size)) {
            return false;
        }
        return vfs_open(out, &f) && !f.is_dir;
    }

    for (int i = 0; command_path[i] != NULL; i++) {
        char joined[PATH_MAX];
        size_t n = 0;
        for (const char *p = command_path[i]; *p != '\0' && n < sizeof joined - 2; p++) {
            joined[n++] = *p;
        }
        joined[n++] = '/';
        for (const char *p = word; *p != '\0' && n < sizeof joined - 1; p++) {
            joined[n++] = *p;
        }
        joined[n] = '\0';

        if (!path_resolve("/", joined, out, size)) {
            continue;
        }
        if (vfs_open(out, &f) && !f.is_dir) {
            return true;
        }
    }
    return false;
}
static void launch(const char *path, int argc, char **argv, bool announce);

/* who is at the keyboard. every program the shell starts inherits it,
 * and nothing in ring 3 can reach in and change it */

/* it is a kernel thread rather than a process, so it keeps its own --
 * and hands it to everything it starts, which is what makes `cd`
 * somewhere and then running something mean what anybody would expect */
/* a session starts here rather than in a static initialiser.
 *
 * that used to be the same thing: one shell, one set of globals, and
 * `= 1` on the job counter was enough. with a session per console the
 * struct starts zeroed, and a zero job number means "no job" everywhere
 * else in this file -- so every session's first job was job zero, which
 * is to say no job at all */
static void session_init(void) {
    shell_cwd[0] = '/';
    shell_cwd[1] = '\0';
    next_job_number = 1;
    current_job = 0;
    hist_count = 0;
    for (int i = 0; i < JOBS_MAX; i++) {
        jobs[i].state = JOB_FREE;
    }
}
static size_t common_prefix(const char *a, const char *b);

/* ---- the personas one may summon ---------------------------------- */

struct persona {
    const char *name;
    const char *line;
    uint64_t    period_ms;
};

static const struct persona personas[] = {
    { "pixie",      "count",   700 },
    { "jack-frost", "hee-ho!", 1300 },
};
#define PERSONA_COUNT (sizeof(personas) / sizeof(personas[0]))

/* how many times a summoned persona speaks before departing. finite on
 * purpose: an immortal thread scribbling over the prompt makes the
 * shell unusable, and watching it exit shows off the reaper anyway */
#define PERSONA_LINES 8

/* ctrl+c bumps this. every persona remembers what it was when it was
 * summoned, and takes the hint when the number moves. I have no
 * signals and no way to yank a sleeping thread off the run queue, so
 * cancelling is cooperative: a persona notices next time it wakes up,
 * which can be up to one sleep period later */
static volatile uint64_t cancel_generation;

static void persona_thread(void *arg) {
    const struct persona *p = arg;
    uint64_t summoned_at = cancel_generation;

    for (int i = 1; i <= PERSONA_LINES; i++) {
        if (cancel_generation != summoned_at) {
            kprintf("[%s] recalled to the velvet room\n", p->name);
            return;
        }
        kprintf("[%s] %s %d, uptime %lums\n", p->name, p->line, i,
                pit_uptime_ms());
        sleep_ms(p->period_ms);
    }
}

/* ---- commands ------------------------------------------------------ */

/* `help <name>`.
 *
 * for a builtin, out of the table. for a program, by *running it* with
 * --help and letting it answer -- which is the only way to get the
 * truth: what a program takes is declared inside the program, and any
 * copy the shell kept would be a second copy, free to drift. */
static void help_one(const char *name) {
    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(name, c->name) != 0) {
            continue;
        }
        kprintf("%s\n", c->usage != NULL ? c->usage : c->name);
        kprintf("  %s\n", c->help);
        kprintf("\n  built into the kernel, so it can change the shell "
                "itself -- which is\n  why `cd` is one and `cat` is not\n");
        return;
    }

    char path[PATH_MAX];
    if (find_program(name, path, sizeof path)) {
        char *args[2];
        args[0] = (char *)name;
        args[1] = (char *)"--help";
        launch(path, 2, args, false);
        return;
    }

    kprintf("'%s' is not something you can type. `help` lists what is\n", name);
}

static void cmd_help(int argc, char **argv) {
    if (argc > 1) {
        help_one(argv[1]);
        return;
    }

    /* names, in columns, and nothing else.
     *
     * this used to print a description beside every one of them, which
     * was a wall of text you had to read all of to find the line you
     * wanted. what somebody scanning this needs is the vocabulary; what
     * one thing means is `help <name>`, and that answer is better than
     * anything that would fit on a shared line anyway */
    size_t columns = 0;
    console_size(&columns, NULL, NULL, NULL);
    size_t per_row = (columns > 20) ? (columns - 4) / 12 : 4;
    if (per_row < 2) {
        per_row = 2;
    }

    kprintf("built in\n ");
    size_t n = 0;
    for (const struct command *c = commands; c->name; c++) {
        kprintf(" %s", c->name);
        for (size_t i = strlen(c->name); i < 11; i++) {
            kprintf(" ");
        }
        if (++n % per_row == 0) {
            kprintf("\n ");
        }
    }
    kprintf("\n\nprograms, in ring 3 with memory of their own\n ");
    n = 0;

    /* the path in order, and a name seen once is not shown again: a
     * program earlier on the path hides one later, exactly as running
     * it would, and saying it twice would suggest otherwise */
    char shown[32][24];
    size_t count = 0;

    for (int d = 0; command_path[d] != NULL; d++) {
        struct vfs_file f;
        for (size_t i = 0; vfs_readdir(command_path[d], i, &f); i++) {
            if (f.is_dir || f.name[0] == '\0') {
                continue;
            }
            bool already = false;
            for (size_t k = 0; k < count; k++) {
                if (strcmp(shown[k], f.name) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            if (count < 32) {
                size_t w = 0;
                while (f.name[w] != '\0' && w < sizeof shown[0] - 1) {
                    shown[count][w] = f.name[w];
                    w++;
                }
                shown[count][w] = '\0';
                count++;
            }
            kprintf(" %s", f.name);
            for (size_t i = strlen(f.name); i < 11; i++) {
                kprintf(" ");
            }
            if (++n % per_row == 0) {
                kprintf("\n ");
            }
        }
    }

    kprintf("\n\n`help <name>` for what one takes -- and for a program "
            "that answer\ncomes from the program itself, so it cannot be "
            "out of date.\n");
    kprintf("looked for in");
    for (int d = 0; command_path[d] != NULL; d++) {
        kprintf(" %s", command_path[d]);
    }
    kprintf(", in that order; a name with a slash is a path.\n");
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();
}

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t total = pmm_total_bytes();
    uint64_t used  = pmm_used_bytes();
    uint64_t freeb = pmm_free_bytes();

    kprintf("physical frames\n");
    kprintf("  total  %lu MiB (%lu frames)\n", total / (1024 * 1024),
            total / PAGE_SIZE);
    kprintf("  used   %lu KiB (%lu frames)\n", used / 1024, used / PAGE_SIZE);
    kprintf("  free   %lu MiB (%lu frames)\n", freeb / (1024 * 1024),
            freeb / PAGE_SIZE);
    kprintf("  peak   %lu KiB ever in use at once\n",
            pmm_peak_bytes() / 1024);
    kprintf("  books  %lu KiB, what the allocator spends on itself\n",
            pmm_metadata_bytes() / 1024);

    /* how much is free matters less than what shape it is in. a machine
     * with megabytes free and none of it contiguous cannot satisfy a
     * large request, and this is the only view that would show it */
    kprintf("free blocks, by size\n ");
    for (unsigned order = 0; order <= 10; order++) {
        uint64_t blocks = pmm_blocks_at(order);
        if (blocks == 0) {
            continue;
        }
        uint64_t kib = (PAGE_SIZE << order) / 1024;
        if (kib < 1024) {
            kprintf(" %lux%luK", blocks, kib);
        } else {
            kprintf(" %lux%luM", blocks, kib / 1024);
        }
    }
    kprintf("\n");

    kprintf("kernel heap\n");
    kprintf("  total  %lu KiB claimed from the pmm\n", kheap_total_bytes() / 1024);
    kprintf("  used   %lu bytes handed out\n", kheap_used_bytes());
}

static void cmd_disk(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!disk_ready()) {
        kprintf("no disk. this machine has only the ramdisk, which is a tar\n");
        kprintf("file limine handed me and which forgets everything on reboot.\n");
        kprintf("give qemu a drive and there will be somewhere to write.\n");
        return;
    }

    kprintf("drive      %s\n", disk_model());
    {
        /* which partition, rather than which drive answered first --
         * which is the difference 0.2.15 made */
        struct disk_part e;
        int at = disk_mounted_part();
        if (at >= 0 && disk_part_at((size_t)at, &e)) {
            if (e.scheme == PART_NONE) {
                kprintf("partition  none -- a filesystem written straight "
                        "to sector zero\n");
            } else {
                kprintf("partition  %d of %lu, %s, starting at sector %lu\n",
                        at, (uint64_t)disk_part_count(),
                        e.scheme == PART_GPT ? "gpt" : "mbr",
                        e.p.first_lba);
            }
        }
    }
    kprintf("capacity   %lu MiB (%lu sectors)\n",
            disk_bytes() / (1024 * 1024), disk_bytes() / AHCI_SECTOR);
    kprintf("filesystem %s, labelled \"%s\", mounted at /\n",
            disk_kind_name(), disk_label());
    if (disk_which() == DISK_FAT32) {
        kprintf("           (fat records no owners and no permissions, so "
                "everything on it\n");
        kprintf("            is 0644 owned by root by decree. an ext2 disk "
                "answers for itself)\n");
    }
    kprintf("%s   %lu bytes each\n",
            disk_which() == DISK_EXT2 ? "blocks  " : "clusters",
            (uint64_t)disk_cluster_bytes());

    uint64_t used = 0, total = 0;
    if (disk_usage(&used, &total)) {
        kprintf("used       %lu KiB of %lu MiB\n",
                used / 1024, total / (1024 * 1024));
    }

    struct bcache_stats c;
    disk_cache_stats(&c);
    uint64_t asked = c.hits + c.misses;
    kprintf("\ncache      %lu of %d blocks held, %lu dirty\n",
            (uint64_t)c.held, BCACHE_BLOCKS, (uint64_t)c.dirty);
    if (asked > 0) {
        /* the hit rate is the only honest measure of whether the cache
         * was worth writing. a filesystem walking a cluster chain asks
         * for the same table sectors over and over, so this number
         * being high is the point rather than a surprise */
        kprintf("           %lu of %lu reads answered without the drive "
                "(%lu%%)\n", c.hits, asked, (c.hits * 100) / asked);
    }
    if (c.writes > 0) {
        kprintf("           %lu writes became %lu trips to the drive\n",
                c.writes, c.writebacks);
    }

    kprintf("\ntry: ls, cat welcome.txt, echo something worth keeping "
            "> /notes.txt\n");
}

/* what a drive says it holds.
 *
 * a disk is not a filesystem, and until 0.2.15 this kernel believed
 * otherwise -- it asked each drive whether sector zero looked like a
 * superblock and mounted whichever answered first. that works exactly
 * as long as every disk has one filesystem starting at the beginning,
 * which is true of nothing anybody uses */
static void cmd_parts(int argc, char **argv) {
    (void)argc; (void)argv;

    size_t n = disk_part_count();
    if (n == 0) {
        kprintf("no drives, or none I could read a sector from\n");
        return;
    }

    kprintf("  #  drive  scheme  start        sectors      kind\n");
    for (size_t i = 0; i < n; i++) {
        struct disk_part e;
        if (!disk_part_at(i, &e)) {
            continue;
        }
        const char *scheme = (e.scheme == PART_GPT) ? "gpt"
                           : (e.scheme == PART_MBR) ? "mbr" : "-";

        kprintf("%s%2lu  %5u  %-6s  %-11lu  %-11lu  %s",
                (int)i == disk_mounted_part() ? " *" : "  ",
                (uint64_t)i, e.p.drive, scheme,
                e.p.first_lba, e.p.sectors, e.p.kind);
        if (e.p.name[0] != '\0') {
            kprintf("  \"%s\"", e.p.name);
        }
        if (e.fs[0] != '\0') {
            kprintf("  [%s]", e.fs);
        }
        kprintf("\n");
    }
    kprintf("\na * is the one mounted at /. `mount <number>` moves it\n");
}

/* what a write-back cache costs, and the thing that pays it back.
 *
 * until this returns, the disk does not hold what the machine says it
 * holds. unix has had this command since 1971 for that reason, and
 * typing it three times before pulling the plug was folklore long
 * before it was a joke */
static void cmd_sync(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!disk_ready()) {
        kprintf("no disk, so there is nothing anywhere to lose\n");
        return;
    }
    if (!disk_dirty()) {
        kprintf("nothing waiting -- the disk already holds what I do\n");
        return;
    }
    if (!disk_sync()) {
        kprintf("the drive refused something. what is on it is not what I "
                "believe\n");
        return;
    }
    kprintf("written\n");
}

static void cmd_cd(int argc, char **argv) {
    /* a builtin, and it has to be: a program runs as its own process
     * with its own working directory, so a `cd` that was a program
     * would change where *it* was standing and then exit */
    const char *want = (argc > 1) ? argv[1] : "/";

    char resolved[PATH_MAX];
    if (!path_resolve(shell_cwd, want, resolved, sizeof resolved)) {
        kprintf("that path is longer than I can hold\n");
        return;
    }

    if (!path_is_root(resolved)) {
        struct vfs_file f;
        if (!vfs_open(resolved, &f)) {
            kprintf("%s: no such place\n", resolved);
            return;
        }
        if (!f.is_dir) {
            kprintf("%s is a file, not somewhere to stand\n", resolved);
            return;
        }
    }

    for (size_t i = 0; i < sizeof shell_cwd; i++) {
        shell_cwd[i] = resolved[i];
        if (resolved[i] == '\0') {
            break;
        }
    }
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("%s\n", shell_cwd);
}

static void cmd_locks(int argc, char **argv) {
    (void)argc; (void)argv;

    kprintf("%-10s %-5s %-6s %s\n", "lock", "rank", "held", "times waited");

    uint64_t total = 0;
    for (size_t i = 0; i < spin_count(); i++) {
        const struct spinlock *l = spin_at(i);
        kprintf("%-10s %-5d %-6s %lu\n", l->name, (int)l->rank,
                l->held ? "yes" : "no", l->contended);
        total += l->contended;
    }

    kprintf("\n%zu locks. ", spin_count());
    if (total == 0) {
        kprintf("nothing has ever waited on one, which is what a\n");
        kprintf("machine running kernel code on one core looks like.\n");
    } else {
        kprintf("waited %lu times in total.\n", total);
    }
    kprintf("\nrank is the order they may be taken in: a lock may only be\n");
    kprintf("taken while holding lower-ranked ones. it is read off the call\n");
    kprintf("graph -- tty calls the scheduler, the scheduler reaches into\n");
    kprintf("the process table, all of them allocate, and anything may\n");
    kprintf("print. two locks taken in opposite orders by two cores is a\n");
    kprintf("machine that stops with nothing to say, so it is checked.\n");
}

static void cmd_cpus(int argc, char **argv) {
    (void)argc; (void)argv;

    size_t n = smp_cpu_count();
    if (n == 0) {
        kprintf("the firmware never said how many processors this machine "
                "has,\nso I am using the one I woke up on.\n");
        return;
    }

    kprintf("%-4s %-6s %-8s %s\n", "cpu", "apic", "state", "running");
    for (size_t i = 0; i < n; i++) {
        const struct cpu *c = smp_cpu_at(i);
        const char *what = "halted";
        if (c->online && c->scheduling) {
            what = sched_cpu_running(c->index);
        } else if (c->online) {
            what = "awake, not scheduling";
        }
        kprintf("%-4u %-6u %-8s %s%s\n", c->index, c->apic_id,
                c->online ? "awake" : "silent", what,
                (c->online && !c->bootstrap && c->reported_id != c->apic_id)
                    ? "  (and reported a different apic id!)" : "");
    }

    kprintf("\n%zu of %zu processors awake, %zu taking work.\n\n",
            smp_online_count(), n, sched_cores_scheduling());
    kprintf("these are cores, not threads. `ps` lists threads and now says\n");
    kprintf("which core each is on -- a thread that is merely ready is on\n");
    kprintf("none of them. there is one run queue and every core picks from\n");
    kprintf("it, so `summon` a few and they land wherever there is room.\n");
}

/* mount a particular partition instead of whichever answered first.
 *
 * this is deliberately blunt: whatever was open on the old one is stale
 * afterwards, because there is no reference counting here that could do
 * better and pretending otherwise would be worse than saying so */
static void cmd_mount_at(int argc, char **argv) {
    size_t which = 0;
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("mount <number> -- `parts` lists them\n");
            return;
        }
        which = which * 10 + (size_t)(*p - '0');
    }

    if (which >= disk_part_count()) {
        kprintf("there is no partition %lu. `parts` lists them\n",
                (uint64_t)which);
        return;
    }
    if ((int)which == disk_mounted_part()) {
        kprintf("partition %lu is already the one at /\n", (uint64_t)which);
        return;
    }

    if (!disk_mount_part(which)) {
        kprintf("nothing I recognise is on partition %lu -- the table says "
                "what it\n", (uint64_t)which);
        kprintf("is *meant* to hold, which is a different question from "
                "what is there\n");
        return;
    }

    kprintf("partition %lu is now at /, holding %s\n",
            (uint64_t)which, disk_kind_name());
    kprintf("anything that was open on the old one is stale. there is no "
            "reference\n");
    kprintf("counting here that could have done better, so this says so "
            "instead\n");
}

static void cmd_mount(int argc, char **argv) {
    if (argc > 1) {
        cmd_mount_at(argc, argv);
        return;
    }
    (void)argc; (void)argv;

    kprintf("%-8s %-7s %-5s %s\n", "at", "kind", "write", "on");

    struct vfs_mount m;
    for (size_t i = 0; vfs_mount_at(i, &m); i++) {
        kprintf("%-8s %-7s %-5s %s%s\n", m.at, m.what,
                m.writable ? "yes" : "no", m.where,
                m.present ? "" : "   (absent)");
    }

    kprintf("\na name with no leading slash is looked for on the disk "
            "first and\n");
    kprintf("%s second, so a disk may supply its own copy of anything "
            "and a\n", VFS_BOOT);
    kprintf("machine without one still finds what it booted with.\n");
}

static void cmd_slabs(int argc, char **argv) {
    (void)argc; (void)argv;

    kprintf("%-14s %6s %6s %6s %6s %6s\n",
            "cache", "size", "/page", "live", "peak", "pages");

    uint64_t held = 0, wanted = 0;
    for (struct slab_cache *c = slab_first_cache(); c != NULL; c = c->next) {
        kprintf("%-14s %6zu %6zu %6zu %6zu %6zu\n",
                c->name, c->obj_size, c->per_slab,
                c->in_use, c->high_water, c->pages);
        held += c->pages * PAGE_SIZE;
        wanted += c->in_use * c->obj_size;
    }

    kprintf("holding %lu KiB for %lu KiB of objects\n",
            held / 1024, wanted / 1024);
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    sched_dump();

    /* a pipe outlives neither end and is freed the moment both let go,
     * so this number should be zero at a prompt. anything else and
     * something died without releasing what it held, which is the one
     * failure mode of the whole arrangement that hides */
    size_t pipes = pipe_count();
    if (pipes > 0) {
        kprintf("%lu pipe%s still open\n", pipes, pipes == 1 ? "" : "s");
    }
}

static void cmd_summon(int argc, char **argv) {
    if (argc < 2) {
        kprintf("summon whom? the register holds:\n");
        for (size_t i = 0; i < PERSONA_COUNT; i++) {
            kprintf("  %s\n", personas[i].name);
        }
        return;
    }

    for (size_t i = 0; i < PERSONA_COUNT; i++) {
        if (strcmp(argv[1], personas[i].name) == 0) {
            struct thread *t = thread_create(personas[i].name, persona_thread,
                                             (void *)&personas[i]);
            if (t == NULL) {
                kprintf("the summoning failed -- no memory for a new soul\n");
                return;
            }
            console_set_colors(COLOR_PROMPT, 0x101018);
            kprintf("I am thou... thou art I...\n");
            kprintf("%s has answered thy call (thread %d)\n",
                    personas[i].name, t->id);
            console_set_colors(COLOR_TEXT, 0x101018);
            return;
        }
    }
    kprintf("no persona by the name '%s' dwells here\n", argv[1]);
}

/* hex, with or without the 0x. returns false if its not a number */
static bool parse_hex(const char *s, uint64_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    if (*s == '\0') {
        return false;
    }
    uint64_t v = 0;
    for (; *s; s++) {
        uint64_t d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return false;
        v = v * 16 + d;
    }
    *out = v;
    return true;
}

static void cmd_vmm(int argc, char **argv) {
    if (argc >= 2) {
        uint64_t addr;
        if (!parse_hex(argv[1], &addr)) {
            kprintf("'%s' is not a hex address\n", argv[1]);
            return;
        }
        vmm_dump(addr);
        return;
    }

    /* no argument: show the shape of the address space by pointing at
     * one thing of each kind. the permission column is the interesting
     * part -- code is r-x, everything else is rw- */
    kprintf("pml4 at %p\n", (void *)vmm_kernel_pml4());

    kprintf("code (this very function):\n");
    vmm_dump((uint64_t)(uintptr_t)cmd_vmm);

    kprintf("a string constant:\n");
    vmm_dump((uint64_t)(uintptr_t)"velvet");

    void *heap = kmalloc(64);
    if (heap != NULL) {
        kprintf("the heap:\n");
        vmm_dump((uint64_t)(uintptr_t)heap);
        kfree(heap);
    }

    uint64_t rsp;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp));
    kprintf("this thread's stack:\n");
    vmm_dump(rsp);

    kprintf("and somewhere nobody lives:\n");
    vmm_dump(0x0000dead00000000ull);
}

/* recurse until the stack runs out. lands on the guard page the vmm
 * left unmapped below every thread stack, which turns what would be
 * silent corruption of the next thread's stack into a clean fault.
 *
 * yes gcc, I know its infinite recursion. thats the entire feature */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static uint64_t eat_stack(uint64_t depth) {
    volatile uint64_t padding[64];
    for (int i = 0; i < 64; i++) {
        padding[i] = depth;
    }
    return padding[0] + eat_stack(depth + 1);
}
#pragma GCC diagnostic pop

static void cmd_stackoverflow(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_colors(COLOR_WARN, 0x101018);
    kprintf("running off the end of this thread's stack on purpose...\n");
    console_set_colors(COLOR_TEXT, 0x101018);
    kprintf("returned %lu, which should have been impossible\n", eat_stack(0));
}

static void cmd_bt(int argc, char **argv) {
    (void)argc; (void)argv;
    /* the same walker a panic uses, just with nothing on fire */
    kbacktrace(0, 0);
}

/* the archive is a flat list of paths and I do not search it, so
 * `hello` will not find `bin/hello`. rather than add a path search --
 * which is magic that surprises you later -- say what they probably
 * meant, if exactly one file ends that way */
static const char *suggest_path(const char *name) {
    struct ramdisk_file f;
    const char *found = NULL;
    int matches = 0;

    for (size_t i = 0; ramdisk_stat(i, &f); i++) {
        const char *p = f.name;
        if (p[0] == '.' && p[1] == '/') {
            p += 2;
        }
        /* the part after the last slash */
        const char *base = p;
        for (const char *q = p; *q; q++) {
            if (*q == '/') {
                base = q + 1;
            }
        }
        if (*base != '\0' && strcmp(base, name) == 0) {
            found = p;
            matches++;
        }
    }
    return matches == 1 ? found : NULL;
}

static void missing(const char *what, const char *name) {
    const char *did = suggest_path(name);
    if (did != NULL) {
        kprintf("%s: no such file '%s'. didst thou mean '%s'?\n",
                what, name, did);
    } else {
        kprintf("%s: no such file '%s'. `ls` shows what there is, "
                "and tab completes it\n", what, name);
    }
}

/* ---- jobs -----------------------------------------------------------
 *
 * a job is one typed line, however many processes that turned out to
 * be. the shell keeps them because it is the only thing that knows what
 * was typed -- the kernel has a group number and some pids, and `[1]+
 * stopped  cat x | wc -l` needs the line.
 *
 * a job is remembered only when there is a reason to: something that
 * ran in the foreground and finished is over, and writing it down would
 * only be a list of everything anybody ever did. what earns a slot is
 * being suspended or being put in the background -- in both cases it is
 * still there and you will want to say so later. */



/* the last one referred to, which is what a bare `fg` means. the same
 * one `+` marks in the listing */

static struct shell_job *job_slot(int number) {
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].state != JOB_FREE && jobs[i].number == number) {
            return &jobs[i];
        }
    }
    return NULL;
}

static struct shell_job *job_remember(const struct job *j, const char *line,
                                      enum job_state state) {
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].state != JOB_FREE) {
            continue;
        }
        jobs[i].state = state;
        jobs[i].number = next_job_number++;
        jobs[i].j = *j;
        size_t n = 0;
        while (line[n] != '\0' && n < LINE_MAX - 1) {
            jobs[i].line[n] = line[n];
            n++;
        }
        jobs[i].line[n] = '\0';
        current_job = jobs[i].number;
        return &jobs[i];
    }
    kprintf("that is more jobs than I can keep track of\n");
    return NULL;
}

static void job_forget(struct shell_job *s) {
    s->state = JOB_FREE;
    if (current_job == s->number) {
        current_job = 0;
        /* whatever is left, most recently started */
        for (int i = 0; i < JOBS_MAX; i++) {
            if (jobs[i].state != JOB_FREE && jobs[i].number > current_job) {
                current_job = jobs[i].number;
            }
        }
    }
}

static void job_print(const struct shell_job *s, const char *what) {
    kprintf("[%d]%c %-8s %s\n", s->number,
            s->number == current_job ? '+' : ' ', what, s->line);
}

/* anything that finished while nobody was looking. called before each
 * prompt, which is where every shell has always reported this -- saying
 * it the instant it happens would scribble over whatever is being
 * typed */
static void jobs_reap(void) {
    for (int i = 0; i < JOBS_MAX; i++) {
        struct shell_job *s = &jobs[i];
        if (s->state != JOB_RUNNING) {
            continue;
        }
        if (user_job_alive(&s->j)) {
            continue;
        }
        user_job_collect(&s->j);
        job_print(s, "done");
        job_forget(s);
    }
}

/* the line as it was typed, kept so a job can be named later. it is
 * copied before the split chops it into words with NULs */

/* what to do with a job that came back from the foreground. either it
 * finished, in which case there is nothing to remember, or ctrl+z
 * stopped it and it wants a number */
static void job_returned(const struct job *j) {
    if (!j->stopped) {
        return;
    }
    struct shell_job *s = job_remember(j, typed_line, JOB_STOPPED);
    if (s != NULL) {
        job_print(s, "stopped");
    }
}

/* start a program, handing it everything after the command name as its
 * arguments. a trailing & means the background */
static void launch(const char *path, int argc, char **argv, bool announce) {
    bool background = (argc > 0 && strcmp(argv[argc - 1], "&") == 0);
    if (background) {
        argc--;
    }

    struct job j;
    const char *why = NULL;
    if (!user_run(path, argc, (const char *const *)argv, shell_cwd,
                  current_uid, background, announce, &j, &why)) {
        if (why == USER_RUN_NO_SUCH_FILE) {
            missing("run", path);
        } else {
            kprintf("cannot run %s: %s\n", path, why);
        }
        return;
    }

    if (background) {
        struct shell_job *s = job_remember(&j, typed_line, JOB_RUNNING);
        if (s != NULL) {
            job_print(s, "running");
        }
        return;
    }
    job_returned(&j);
}

/* ---- the three commands job control is for ------------------------- */

static void cmd_jobs(int argc, char **argv) {
    (void)argc; (void)argv;

    int shown = 0;
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].state == JOB_FREE) {
            continue;
        }
        job_print(&jobs[i],
                  jobs[i].state == JOB_STOPPED ? "stopped" : "running");
        shown++;
    }
    if (shown == 0) {
        kprintf("nothing is waiting\n");
    }
}

/* which job a command like `fg 2` means. no number means the one marked
 * `+`, which is the one you last touched -- that is what makes a bare
 * `fg` the useful thing it is */
static struct shell_job *job_named(int argc, char **argv, const char *who) {
    int number = current_job;

    if (argc > 1) {
        number = 0;
        const char *p = argv[1];
        if (*p == '%') {
            p++;               /* `%1` is how everybody else spells it */
        }
        for (; *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                kprintf("%s <number> -- `jobs` lists them\n", who);
                return NULL;
            }
            number = number * 10 + (*p - '0');
        }
    }

    if (number == 0) {
        kprintf("nothing is waiting\n");
        return NULL;
    }

    struct shell_job *s = job_slot(number);
    if (s == NULL) {
        kprintf("there is no job %d\n", number);
    }
    return s;
}

static void cmd_fg(int argc, char **argv) {
    struct shell_job *s = job_named(argc, argv, "fg");
    if (s == NULL) {
        return;
    }

    kprintf("%s\n", s->line);       /* say what is coming back */
    current_job = s->number;

    user_job_continue(&s->j, true);
    if (user_job_wait(&s->j)) {
        job_forget(s);              /* it finished this time */
        return;
    }

    /* stopped again. it keeps its number, which is what makes ctrl+z,
     * fg, ctrl+z, fg work without the numbers wandering */
    s->state = JOB_STOPPED;
    job_print(s, "stopped");
}

static void cmd_bg(int argc, char **argv) {
    struct shell_job *s = job_named(argc, argv, "bg");
    if (s == NULL) {
        return;
    }
    if (s->state == JOB_RUNNING) {
        kprintf("[%d] is already running\n", s->number);
        return;
    }

    current_job = s->number;
    s->state = JOB_RUNNING;
    user_job_continue(&s->j, false);
    job_print(s, "running");

    /* it runs, but the keyboard is not its any more. a read from it
     * comes back -1 rather than taking keys from whoever is being typed
     * at -- unix stops the process instead, which is tidier and wants a
     * signal I do not have */
}

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        kprintf("run <program> [args...] [&] -- try `run bin/hello`\n");
        return;
    }
    launch(argv[1], argc - 1, argv + 1, true);
}

/* a live picture, redrawn until somebody presses a key. everything it
 * shows already existed -- the point of this version is that a number
 * you can watch move tells you something a number you have to ask for
 * twice does not */
/* the half of 0.1.7 that cannot be proved before it is trusted, so it
 * is asked for from a shell that already works rather than done to you
 * at boot. if the keyboard goes quiet afterwards, a reboot undoes it */
static void cmd_lspci(int argc, char **argv) {
    (void)argc; (void)argv;

    if (pci_count() == 0) {
        kprintf("nothing answered on the pci bus\n");
        return;
    }

    for (size_t i = 0; ; i++) {
        const struct pci_device *d = pci_at(i);
        if (d == NULL) {
            break;
        }

        kprintf("  %02x:%02x.%u  %04x:%04x  %s",
                d->bus, d->slot, d->function, d->vendor, d->device,
                pci_class_name(d->class_code, d->subclass));

        const char *name = pci_device_name(d->vendor, d->device);
        const char *maker = pci_vendor_name(d->vendor);
        if (name != NULL) {
            kprintf("  -- %s", name);
        } else if (maker != NULL) {
            kprintf("  -- %s, model unknown to me", maker);
        }
        kprintf("\n");

        /* where it listens. a device with no bars is one that is
         * spoken to some other way, which is worth seeing too */
        bool any_bar = false;
        for (size_t b = 0; b < 6; b++) {
            struct pci_bar bar = pci_decode_bar(d->bar[b]);
            if (bar.address == 0) {
                continue;
            }
            if (!any_bar) {
                kprintf("           ");
                any_bar = true;
            }
            kprintf(" bar%zu=%s%p%s", b, bar.is_io ? "io " : "mem ",
                    (void *)bar.address, bar.is_64bit ? " (64-bit)" : "");
        }
        if (any_bar) {
            kprintf("\n");
        }
        if (d->irq_line != 0 && d->irq_line != 0xff) {
            kprintf("            irq %u\n", d->irq_line);
        }
    }
    kprintf("  %zu devices\n", pci_count());
}

static void cmd_ioapic(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!interrupts_on_apic()) {
        kprintf("interrupts are still on the 8259; there is nothing to "
                "move them from\n");
        return;
    }

    kprintf("moving the keyboard and serial onto the io apic.\n");
    kprintf("if this was a mistake, the keyboard will simply stop and a\n");
    kprintf("reboot will put everything back.\n\n");

    if (interrupts_use_ioapic()) {
        kprintf("\npress a key. if this echoes, it worked.\n");
    }
}

static void cmd_top(int argc, char **argv) {
    (void)argc; (void)argv;

    while (!input_haskey()) {
        console_clear();

        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("tinyOS %s -- press any key to stop watching\n\n", VERSION);
        console_set_colors(COLOR_TEXT, 0x101018);

        uint64_t ms = pit_uptime_ms();
        kprintf("up %luh %lum %lus     %zu threads, %zu processes\n",
                ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60,
                sched_thread_count(), process_count());

        kprintf("memory  %lu / %lu MiB in use, peaked at %lu MiB\n",
                pmm_used_bytes() / (1024 * 1024),
                pmm_total_bytes() / (1024 * 1024),
                pmm_peak_bytes() / (1024 * 1024));
        kprintf("heap    %lu KiB claimed, %lu bytes handed out\n\n",
                kheap_total_bytes() / 1024, kheap_used_bytes());

        sched_dump();

        /* which doors ring 3 actually uses. a syscall nobody calls is
         * worth knowing about too, so the unused ones are left out
         * rather than listed as zero */
        kprintf("\nsyscalls\n ");
        bool any = false;
        for (unsigned i = 0; i < SYSCALL_COUNT; i++) {
            uint64_t n = syscall_times_called(i);
            if (n > 0) {
                kprintf(" %s=%lu", syscall_name(i), n);
                any = true;
            }
        }
        kprintf("%s\n", any ? "" : " none yet");

        sleep_ms(500);
    }

    (void)input_getchar();      /* the key that stopped me is not a command */
    console_clear();
}

static void cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    const struct account *a = auth_find(current_user);
    kprintf("%s, uid %d%s%s\n", current_user, current_uid,
            (a != NULL && a->description[0]) ? " -- " : "",
            (a != NULL) ? a->description : "");
    if (current_uid == 0) {
        kprintf("the velvet room answers to thee\n");
    }
}

static void cmd_logout(int argc, char **argv);

static void cmd_dmesg(int argc, char **argv) {
    (void)argc; (void)argv;
    /* everything the boot said while the screen was being kept quiet */
    klog_dump();
}

/* ---- who and what I am -------------------------------------------- */

static void cmd_arcana(int argc, char **argv) {
    (void)argc; (void)argv;

    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("\nThou art I... And I am thou...\n\n");
    console_set_colors(COLOR_TEXT, 0x101018);

    kprintf("  THE COMPUTER ARCANA\n");
    kprintf("  rank %s -- the bond deepens with every commit\n\n", VERSION);

    kprintf("  version    tinyOS %s\n", VERSION);
    kprintf("  forged     %s, %s\n", __DATE__, __TIME__);
    kprintf("  by         gcc %s\n", __VERSION__);
    kprintf("  known      %lu functions by name\n", ksym_count);
    if (ramdisk_present()) {
        kprintf("  carrying   %zu files in the ramdisk\n", ramdisk_count());
    }
    kprintf("\n");
}

/* fastfetch, if fastfetch had read a tarot deck */
static void cmd_persona(int argc, char **argv) {
    (void)argc; (void)argv;

    static const char *mask[] = {
        "     .-\"\"\"\"\"-.     ",
        "   .'  _     _  '.   ",
        "  /   (o)   (o)   \\  ",
        " |       ---       | ",
        " |    \\  ___  /    | ",
        "  \\    '.___.'    /  ",
        "   '.           .'   ",
        "     '-._____.-'     ",
    };

    char brand[49];
    cpu_brand(brand);

    size_t cols = 0, rows = 0, w = 0, h = 0;
    console_size(&cols, &rows, &w, &h);

    uint64_t ms = pit_uptime_ms();
    uint64_t total = pmm_total_bytes() / (1024 * 1024);
    uint64_t used  = pmm_used_bytes() / (1024 * 1024);

    /* the info column, one line per line of the mask */
    const int LINES = 8;
    for (int i = 0; i < LINES; i++) {
        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("%s", mask[i]);
        console_set_colors(COLOR_TEXT, 0x101018);

        switch (i) {
        case 0:
            kprintf("velvet@tinyOS");
            break;
        case 1:
            kprintf("-------------");
            break;
        case 2:
            kprintf("arcana    the Computer, rank %s", VERSION);
            break;
        case 3:
            kprintf("persona   %s", brand);
            break;
        case 4:
            kprintf("awakened  %luh %lum %lus",
                    ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60);
            break;
        case 5:
            kprintf("souls     %zu threads bound to the wheel",
                    sched_thread_count());
            break;
        case 6:
            kprintf("memory    %lu / %lu MiB", used, total);
            break;
        case 7:
            kprintf("vision    %zux%zu (%zux%zu of glyphs)", w, h, cols, rows);
            break;
        }
        kprintf("\n");
    }
    kprintf("\n");
}

static void cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *months[] = { "", "january", "february", "march",
        "april", "may", "june", "july", "august", "september", "october",
        "november", "december" };

    struct rtc_time t;
    rtc_read(&t);
    kprintf("%02u:%02u:%02u on the %u%s of %s, %u\n",
            t.hour, t.minute, t.second, t.day,
            (t.day / 10 == 1) ? "th"
              : (t.day % 10 == 1) ? "st"
              : (t.day % 10 == 2) ? "nd"
              : (t.day % 10 == 3) ? "rd" : "th",
            months[t.month <= 12 ? t.month : 0], t.year);
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) {
        kprintf("hexdump <hex address> [bytes]\n");
        return;
    }
    uint64_t addr, count = 64;
    if (!parse_hex(argv[1], &addr)) {
        kprintf("'%s' is not a hex address\n", argv[1]);
        return;
    }
    if (argc >= 3 && !parse_hex(argv[2], &count)) {
        kprintf("'%s' is not a hex length\n", argv[2]);
        return;
    }
    if (count > 1024) {
        count = 1024;       /* you did not mean that */
    }

    for (uint64_t off = 0; off < count; off += 16) {
        uint64_t base = addr + off;

        /* ask the page tables before touching anything. a hexdump that
         * page faults on a typo would be a poor debugging tool */
        if (vmm_translate(vmm_kernel_pml4(), base) == VMM_NO_MAPPING) {
            kprintf("%p  <not mapped>\n", (void *)base);
            continue;
        }

        const unsigned char *p = (const unsigned char *)base;
        kprintf("%p ", (void *)base);
        for (int i = 0; i < 16; i++) {
            kprintf(" %02x", p[i]);
        }
        kprintf("  ");
        for (int i = 0; i < 16; i++) {
            kprintf("%c", (p[i] >= ' ' && p[i] <= '~') ? p[i] : '.');
        }
        kprintf("\n");
    }
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) {
        kprintf("kill <thread id> -- see `ps`\n");
        return;
    }
    uint64_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("'%s' is not a thread id\n", argv[1]);
            return;
        }
        id = id * 10 + (uint64_t)(*p - '0');
    }
    switch (sched_kill((int)id)) {
    case SCHED_KILL_OK:
        kprintf("thread %lu returns to the sea of souls\n", id);
        break;
    case SCHED_KILL_NO_SUCH:
        kprintf("no thread %lu walks this realm\n", id);
        break;
    case SCHED_KILL_SELF:
        kprintf("i will not unmake myself while thou art still speaking\n");
        break;
    case SCHED_KILL_PROTECTED:
        kprintf("that one keeps the wheel turning. leave it be\n");
        break;
    }
}

static void cmd_history(int argc, char **argv);   /* needs the history array */

static void cmd_time(int argc, char **argv) {
    if (argc < 2) {
        kprintf("time <command> -- how long it takes\n");
        return;
    }
    uint64_t start = pit_uptime_ms();
    run_argv(argc - 1, argv + 1);
    kprintf("[%lums]\n", pit_uptime_ms() - start);
}

/* both of the ways this machine stops have to write first.
 *
 * before 0.2.13 a write reached the drive as it was made, so a reboot
 * lost nothing by definition. it is a cache now, and a reboot that does
 * not sync throws away whatever had not been written yet -- which on
 * this machine is usually the file somebody just spent a minute editing */
static void settle(void) {
    if (disk_ready() && disk_dirty()) {
        kprintf("writing what is still in memory...\n");
        if (!disk_sync()) {
            kprintf("the drive refused. something is being lost here\n");
        }
    }
}

static void cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    settle();
    system_poweroff();
}

static void cmd_crash(int argc, char **argv) {
    (void)argc; (void)argv;
    console_set_colors(COLOR_WARN, 0x101018);
    kprintf("tempting fate: reading from 0xdeadbeef...\n");
    console_set_colors(COLOR_TEXT, 0x101018);

    volatile uint64_t *bad = (volatile uint64_t *)0xdeadbeef;
    uint64_t got = *bad;

    kprintf("read back %lx -- which should have been impossible\n", got);
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    settle();
    reboot();
}

/* switch screens from the keyboard-less side of the machine.
 *
 * alt+f1..f4 does this from a keyboard, and a keyboard is the obvious
 * way -- but somebody on the serial line has no alt key and no function
 * keys, and telling them the feature is not for them would be a strange
 * thing for a kernel to decide */
static void cmd_chvt(int argc, char **argv) {
    if (argc < 2) {
        kprintf("this is console %u of %d.\n",
                tty_my_console() + 1, VCONSOLE_COUNT);
        kprintf("  chvt <n>        show another one\n");
        kprintf("  alt+1 .. alt+4  the same, from a keyboard\n");
        kprintf("  alt+f1 .. f4    also, where the host does not eat "
                "them first\n");
        kprintf("  ctrl+\\ then n   the same, over a serial line\n");
        kprintf("  shift+pageup    look back up this one\n");
        return;
    }

    int n = 0;
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("chvt <n>, where n is 1 to %d\n", VCONSOLE_COUNT);
            return;
        }
        n = n * 10 + (*p - '0');
    }
    if (n < 1 || n > VCONSOLE_COUNT) {
        kprintf("there are %d consoles, numbered from 1\n", VCONSOLE_COUNT);
        return;
    }

    console_switch((unsigned)(n - 1));

    /* said on *this* console, which is the one nobody is looking at any
     * more -- so it is there when they come back rather than printed
     * over whatever they switched to */
    kprintf("showing console %d\n", n);
}

static const struct command commands[] = {
    { "help",   "list what thou may command",           cmd_help, false, "help [name]" },
    { "clear",  "wipe the screen clean",                cmd_clear, false, NULL },
    { "run",    "give a program the outer ring; & for background", cmd_run, true, "run <program> [args...] [&]" },
    { "whoami", "who thou art, and what that permits",  cmd_whoami, false, NULL },
    { "logout", "leave, and let somebody else in",      cmd_logout, false, NULL },
    { "dmesg",  "everything boot said while you werent looking", cmd_dmesg, false, NULL },
    { "arcana", "the rank of this bond, and its making", cmd_arcana, false, NULL },
    { "persona","the face this machine wears",          cmd_persona, false, NULL },
    { "mem",    "frames and heap, honestly counted",    cmd_mem, false, NULL },
    { "ps",     "the threads that walk this realm",     cmd_ps, false, NULL },
    { "jobs",   "what thou hast set aside",             cmd_jobs, false, NULL },
    { "fg",     "bring one back to the front",          cmd_fg, false, "fg [number]" },
    { "bg",     "let one carry on without the keyboard", cmd_bg, false, "bg [number]" },
    { "top",    "the same, but watched rather than asked", cmd_top, false, NULL },
    { "lspci",  "what is plugged into this machine",    cmd_lspci, false, NULL },
    { "slabs",  "the object caches, and what they hold", cmd_slabs, false, NULL },
    { "disk",   "the drive, and the filesystem on it",  cmd_disk, false, NULL },
    { "sync",   "put what is in memory onto the disk",  cmd_sync, false, NULL },
    { "mount",  "which filesystem is where; mount <n> moves it", cmd_mount, false, "mount [number]" },
    { "parts",  "what each drive says it holds",        cmd_parts, false, NULL },
    { "chvt",   "show another console; alt+f1..f4 too", cmd_chvt, false, "chvt [1-4]" },
    { "cpus",   "the processors, and which are awake",  cmd_cpus, false, NULL },
    { "locks",  "what guards what, and what waits",     cmd_locks, false, NULL },
    { "cd",     "go somewhere; no argument means the root", cmd_cd, true, "cd [directory]" },
    { "pwd",    "where I am standing",                  cmd_pwd, false, NULL },
    { "ioapic", "move external interrupts off the 8259 (risky)", cmd_ioapic, false, NULL },
    { "summon", "call forth a persona thread (in the background)", cmd_summon, false, "summon <name>" },
    { "vmm",    "what the page tables say about an address", cmd_vmm, false, "vmm <address>" },
    { "bt",     "who called whom to get here",          cmd_bt, false, NULL },
    { "date",   "what the battery-backed clock believes", cmd_date, false, NULL },
    { "hexdump","look at memory, safely",                cmd_hexdump, false, "hexdump <address>" },
    { "kill",   "end a thread by id",                    cmd_kill, false, "kill <id>" },
    { "history","what thou hast said before",            cmd_history, false, NULL },
    { "time",   "how long a command takes",              cmd_time, false, "time <command...>" },
    { "crash",  "tempt fate with a wild pointer",       cmd_crash, false, NULL },
    { "smash",  "run off the end of the stack on purpose", cmd_stackoverflow, false, NULL },
    { "reboot", "sever the bond and begin anew",        cmd_reboot, false, NULL },
    { "poweroff","let the velvet room fade",             cmd_poweroff, false, NULL },
    { NULL, NULL, NULL, false, NULL },
};

/* ---- the line editor ----------------------------------------------- */

/* chop a line into argv in place. spaces become terminators, runs of
 * them collapse, and I stop early rather than overflow argv.
 *
 * the four that join commands together -- `|`, `<`, `>`, `>>` -- are
 * their own words whether or not anybody put spaces round them, so
 * `cat x|head` and `echo hi>f` split the same way as the spaced-out
 * spellings. that is one of those things nobody notices working and
 * everybody notices missing.
 *
 * they cannot stay in the line: the word before one needs a terminator,
 * and that terminator goes exactly where the character was. so each is
 * replaced by a NUL and a pointer to a constant is put in argv instead.
 * having seen it is enough; nothing needs the original byte. */
static char *punctuation(char *p, size_t *eaten) {
    static char bar[] = "|", in[] = "<", out[] = ">", app[] = ">>";

    if (*p == '|') { *eaten = 1; return bar; }
    if (*p == '<') { *eaten = 1; return in; }
    if (*p == '>') {
        if (p[1] == '>') { *eaten = 2; return app; }
        *eaten = 1;
        return out;
    }
    *eaten = 0;
    return NULL;
}

static int split(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;

    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0' || argc == max) {
            break;
        }

        size_t eaten;
        char *punct = punctuation(p, &eaten);
        if (punct != NULL) {
            argv[argc++] = punct;
            p += eaten;
            continue;
        }

        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && punctuation(p, &eaten) == NULL) {
            p++;
        }
        if (*p == ' ') {
            *p++ = '\0';
        } else if (*p != '\0') {
            /* punctuation right up against the word. it becomes the
             * terminator, and the loop puts it in argv on the next pass
             * -- which works because `punctuation` reads the character
             * before this one overwrites it */
            punct = punctuation(p, &eaten);
            *p = '\0';
            p += eaten;
            if (argc < max) {
                argv[argc++] = punct;
            }
        }
    }
    return argc;
}

/* is this word a builtin? */
static const struct command *builtin_named(const char *name) {
    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(name, c->name) == 0) {
            return c;
        }
    }
    return NULL;
}

/* dispatch an already-split command. separate from run_line so `time`
 * can hand me its own argv without re-parsing anything */
static void run_argv(int argc, char **argv) {
    if (argc == 0) {
        return;
    }
    {
        const struct command *c = builtin_named(argv[0]);
        if (c != NULL) {
            c->fn(argc, argv);
            return;
        }
    }
    /* not a builtin. before deciding it is nothing, go and look for a
     * program of that name -- on the search path if it is a bare word,
     * or exactly where it says if it has a slash in it */
    {
        char path[PATH_MAX];
        if (find_program(argv[0], path, sizeof path)) {
            /* typed by name rather than through `run`: they want the
             * program's output, not a commentary on it */
            launch(path, argc, argv, false);
            return;
        }
    }

    /* before giving up, see if they nearly typed something real. I
     * only compare leading characters -- enough to catch a fumbled
     * ending like `dmseg`, and honest about not being spell check */
    const struct command *near = NULL;
    size_t best = 0;
    int ties = 0;
    for (const struct command *c = commands; c->name; c++) {
        size_t n = common_prefix(c->name, argv[0]);
        if (n > best) {
            best = n;
            near = c;
            ties = 1;
        } else if (n == best && best > 0) {
            ties++;
        }
    }

    if (best >= 2 && ties == 1) {
        kprintf("'%s' means nothing to me. didst thou mean '%s'?\n",
                argv[0], near->name);
    } else {
        kprintf("'%s' means nothing to me. try 'help'\n", argv[0]);
    }
}

/* ---- pipelines and redirection --------------------------------------
 *
 * a bar separates two commands and joins them at the same time; an
 * arrow moves one end of one command somewhere else. the shell's whole
 * job is the wiring: resolve each name to a program, take the arrows
 * out of the arguments, and hand the list to user_pipeline.
 *
 * builtins cannot be in any of it. `ps > out.txt` would want the
 * shell's own output to go somewhere other than the console, and the
 * shell is a kernel thread with no stdout to redirect -- it prints with
 * kprintf, straight at the screen. that is a real limitation and it is
 * worth saying out loud rather than failing strangely. */

/* pull `< name`, `> name` and `>> name` out of one stage's words,
 * leaving the words that are really arguments.
 *
 * they are removed rather than passed on, because a program has no
 * business seeing them: `sort < a.txt` should look to sort exactly like
 * `sort` with something on its standard input, which is the whole idea.
 *
 * returns false, with something to say, if an arrow has no name after
 * it -- `cat >` is a sentence that stops halfway through. */
static bool take_redirects(struct stage *st, const char **error) {
    int kept = 0;

    for (int i = 0; i < st->argc; i++) {
        const char *w = st->argv[i];
        bool in = (strcmp(w, "<") == 0);
        bool out = (strcmp(w, ">") == 0);
        bool app = (strcmp(w, ">>") == 0);

        if (!in && !out && !app) {
            st->argv[kept++] = st->argv[i];
            continue;
        }
        if (i + 1 >= st->argc) {
            *error = "there is nothing after that arrow to name a file";
            return false;
        }

        if (in) {
            st->in_path = st->argv[i + 1];
        } else {
            st->out_path = st->argv[i + 1];
            st->append = app;
        }
        i++;        /* the filename went with the arrow */
    }

    st->argc = kept;
    if (st->argc == 0) {
        *error = "that is a file with no command to put in it";
        return false;
    }
    return true;
}

/* chop an already-split argv at every bare `|`, in place. each piece
 * comes back as its own argc/argv. returns how many pieces, or -1 if
 * the line is malformed */
static int split_pipeline(int argc, char **argv, struct stage *out, int max) {
    int count = 0;
    int start = 0;

    for (int i = 0; i <= argc; i++) {
        bool bar = (i < argc && strcmp(argv[i], "|") == 0);
        if (i != argc && !bar) {
            continue;
        }
        if (i == start) {
            /* `| x`, `x |`, or `x || y` -- an empty command either side
             * of a bar, which means nothing at all */
            return -1;
        }
        if (count == max) {
            return -1;
        }
        memset(&out[count], 0, sizeof out[count]);
        out[count].argc = i - start;
        out[count].argv = &argv[start];
        count++;
        start = i + 1;
    }
    return count;
}

static void run_pipeline(int argc, char **argv, bool background) {
    struct stage stages[PIPELINE_MAX];
    int count = split_pipeline(argc, argv, stages, PIPELINE_MAX);

    if (count < 0) {
        kprintf("a bar joins two commands, so it wants one on either side "
                "of it\n");
        kprintf("(and I can join at most %d)\n", PIPELINE_MAX);
        return;
    }

    for (int i = 0; i < count; i++) {
        const char *why = NULL;
        if (!take_redirects(&stages[i], &why)) {
            kprintf("%s\n", why);
            return;
        }
    }

    /* the middle of a pipeline already has both ends spoken for, so
     * redirecting one is asking for two different things in the same
     * slot. saying which one would win is worse than refusing */
    for (int i = 0; i < count; i++) {
        if (i > 0 && stages[i].in_path != NULL) {
            kprintf("'%s' already takes its input from the command before "
                    "it\n", stages[i].argv[0]);
            return;
        }
        if (i < count - 1 && stages[i].out_path != NULL) {
            kprintf("'%s' already sends its output to the command after "
                    "it\n", stages[i].argv[0]);
            return;
        }
    }

    /* every stage has to be a program before any of them starts.
     * finding out halfway through would leave the earlier ones already
     * running and writing into a pipe with nobody at the end */
    static char paths[PIPELINE_MAX][PATH_MAX];
    for (int i = 0; i < count; i++) {
        const char *name = stages[i].argv[0];

        const struct command *b = builtin_named(name);
        if (b != NULL) {
            kprintf("'%s' is a builtin, and builtins cannot go in a "
                    "pipeline --\n", name);
            kprintf("the shell is a kernel thread and prints straight at the "
                    "screen, so it\n");
            kprintf("has no output to hand anybody. only programs can be "
                    "joined up.\n");
            return;
        }
        if (!find_program(name, paths[i], sizeof paths[i])) {
            missing("run", name);
            return;
        }
        stages[i].path = paths[i];
    }

    struct job j;
    const char *why = "";
    if (!user_pipeline(stages, count, shell_cwd, current_uid, background,
                       &j, &why)) {
        kprintf("cannot run it: %s\n", why);
        return;
    }

    if (background) {
        struct shell_job *s = job_remember(&j, typed_line, JOB_RUNNING);
        if (s != NULL) {
            job_print(s, "running");
        }
        return;
    }
    job_returned(&j);
}

/* does this line need the pipeline machinery? a bar or an arrow means
 * yes -- and a plain command with neither goes the old way, which is
 * the one that knows how to put something in the background */
static bool needs_wiring(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0 || strcmp(argv[i], "<") == 0
            || strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
            return true;
        }
    }
    return false;
}

static void run_line(char *line) {
    char *argv[ARGV_MAX];

    /* kept before the split, which chops the line into words with NULs
     * written over the spaces. a job has to be able to say what it was */
    size_t n = 0;
    while (line[n] != '\0' && n < LINE_MAX - 1) {
        typed_line[n] = line[n];
        n++;
    }
    typed_line[n] = '\0';

    int argc = split(line, argv, ARGV_MAX);

    if (needs_wiring(argc, argv)) {
        /* a trailing & belongs to the whole line rather than to the
         * last stage, so it comes off before anything is chopped up */
        bool background = (argc > 0 && strcmp(argv[argc - 1], "&") == 0);
        if (background) {
            argc--;
        }
        run_pipeline(argc, argv, background);
        return;
    }
    run_argv(argc, argv);   /* argc 0 just means they pressed enter */
}

static void prompt(void) {
    /* anything that finished while nobody was looking gets reported
     * here rather than the instant it happens -- saying it immediately
     * would scribble over whatever is half-typed */
    jobs_reap();

    console_set_colors(COLOR_PROMPT, 0x101018);
    /* the console number is in the prompt because with four of them
     * looking identical, knowing which one you are typing at is not a
     * thing to have to remember */
    kprintf("%s@velvet[%u]:%s%s ", current_user, tty_my_console() + 1,
            shell_cwd, current_uid == 0 ? "#" : "$");
    console_set_colors(COLOR_TEXT, 0x101018);
}

/* ---- history -------------------------------------------------------- */


static void history_add(const char *line) {
    if (line[0] == '\0') {
        return;             /* dont remember the user pressing enter */
    }
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0) {
        return;             /* dont remember the same thing twice running */
    }

    if (hist_count == HISTORY_MAX) {
        /* oldest falls off the end */
        for (int i = 1; i < HISTORY_MAX; i++) {
            for (int j = 0; j < LINE_MAX; j++) {
                history[i - 1][j] = history[i][j];
            }
        }
        hist_count--;
    }

    size_t n = 0;
    while (line[n] && n < LINE_MAX - 1) {
        history[hist_count][n] = line[n];
        n++;
    }
    history[hist_count][n] = '\0';
    hist_count++;
}

static void cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < hist_count; i++) {
        kprintf("  %2d  %s\n", i + 1, history[i]);
    }
}

/* ---- the visible cursor --------------------------------------------
 * the console treats \b as pure cursor-left now, same as any terminal,
 * so these work identically on the framebuffer and down the wire */

static void move_left(size_t n) {
    while (n-- > 0) {
        kprintf("\b");
    }
}

/* reprint everything from pos onward, plus a space to cover a character
 * that just shifted off the end, then come back to where I was */
static void redraw_tail(const char *line, size_t len, size_t pos) {
    for (size_t i = pos; i < len; i++) {
        kprintf("%c", line[i]);
    }
    kprintf(" ");
    move_left(len - pos + 1);
}

/* throw away what is on screen and put something else there. used by
 * the history keys, which replace the whole line at once */
static void replace_line(char *line, size_t *len, size_t *pos, const char *with) {
    move_left(*pos);                    /* back to the start of the line */
    size_t old = *len;

    size_t n = 0;
    while (with[n] && n < LINE_MAX - 1) {
        line[n] = with[n];
        kprintf("%c", with[n]);
        n++;
    }
    line[n] = '\0';

    for (size_t i = n; i < old; i++) {  /* cover whatever was longer */
        kprintf(" ");
    }
    move_left(old > n ? old - n : 0);

    *len = n;
    *pos = n;
}

static bool is_word_char(char c) {
    return c != ' ';
}

/* ---- tab completion ------------------------------------------------ */

/* how many leading characters two strings share */
static size_t common_prefix(const char *a, const char *b) {
    size_t n = 0;
    while (a[n] != '\0' && a[n] == b[n]) {
        n++;
    }
    return n;
}

/* the word the cursor is sitting in, and whether it is in command
 * position -- meaning a program name is what belongs there, rather than
 * a filename. returns where that word starts.
 *
 * a bar resets that, and has to: in `cat x | he` the `he` is a command,
 * not a file, and completing it against the working directory would
 * offer exactly the wrong list. so the search backwards stops at the
 * most recent bar as well as at the start of the line. */
static size_t word_start(const char *line, size_t pos, bool *first_word) {
    size_t start = pos;
    while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '|') {
        start--;
    }

    *first_word = true;
    for (size_t i = start; i-- > 0; ) {
        if (line[i] == '|') {
            break;              /* everything back to the bar was space */
        }
        if (line[i] != ' ') {
            *first_word = false;
            break;
        }
    }
    return start;
}

/* replace the word under the cursor with `with`, redrawing what follows */
static void replace_word(char *line, size_t *len, size_t *pos,
                         size_t start, const char *with) {
    size_t old_word = *pos - start;
    size_t tail_len = *len - *pos;
    size_t new_word = strlen(with);

    if (start + new_word + tail_len + 1 >= LINE_MAX) {
        return;
    }

    /* shuffle whatever came after the word out of the way */
    for (size_t i = 0; i < tail_len; i++) {
        line[start + new_word + i] = line[*pos + i];
    }
    for (size_t i = 0; i < new_word; i++) {
        line[start + i] = with[i];
    }
    *len = start + new_word + tail_len;
    line[*len] = '\0';

    move_left(old_word);
    for (size_t i = start; i < *len; i++) {
        kprintf("%c", line[i]);
    }
    /* blank anything the shorter word left behind */
    size_t was = start + old_word + tail_len;
    for (size_t i = *len; i < was; i++) {
        kprintf(" ");
    }
    *pos = start + new_word;
    move_left((*len > was ? *len : was) - *pos);
}

/* does the line start with the name of a program? the same question
 * run_argv asks, answered the same way, so completion offers filenames
 * after exactly the words that will actually run something */
static bool first_word_is_program(const char *line) {
    size_t i = 0;
    while (line[i] == ' ') i++;
    size_t start = i;
    while (line[i] != '\0' && line[i] != ' ') i++;

    size_t n = i - start;
    if (n == 0 || n >= PATH_MAX) {
        return false;
    }

    char word[PATH_MAX];
    for (size_t k = 0; k < n; k++) {
        word[k] = line[start + k];
    }
    word[n] = '\0';

    char path[PATH_MAX];
    return find_program(word, path, sizeof path);
}

/* which command the line begins with, or NULL if it is not one I know.
 * the line must already be terminated -- complete() sees to that */
static const struct command *command_for_line(const char *line) {
    size_t i = 0;
    while (line[i] == ' ') {
        i++;
    }
    size_t start = i;
    while (line[i] != '\0' && line[i] != ' ') {
        i++;
    }
    size_t n = i - start;

    for (const struct command *c = commands; c->name; c++) {
        if (strlen(c->name) == n && memcmp(c->name, line + start, n) == 0) {
            return c;
        }
    }
    return NULL;
}

/* candidates come from one of three places depending on where you are:
 * command names in the first word, ramdisk filenames after a command
 * that takes one, and -- once a word starts looking like a path -- the
 * disk, which unlike the ramdisk is a real tree and has to be walked a
 * directory at a time.
 *
 * they are whole words rather than bare names, because a whole word is
 * what gets replaced: completing `notes` in `cat /disk/no` has to put
 * back `/disk/notes/`, not `notes` */
#define MAX_CANDIDATES 24
#define CAND_MAX       96

struct candidates {
    char items[MAX_CANDIDATES][CAND_MAX];
    int  count;
};

static void add_candidate(struct candidates *c, const char *dir,
                          const char *name, bool is_dir) {
    if (c->count >= MAX_CANDIDATES) {
        return;
    }
    char *out = c->items[c->count];
    size_t n = 0;

    while (*dir != '\0' && n < CAND_MAX - 3) {
        out[n++] = *dir++;
    }
    if (name != NULL) {
        if (n > 0 && out[n - 1] != '/' && n < CAND_MAX - 3) {
            out[n++] = '/';
        }
        while (*name != '\0' && n < CAND_MAX - 3) {
            out[n++] = *name++;
        }
    }
    /* a directory gets a slash, so tab again carries straight on into
     * it rather than stopping at a name you cannot open */
    if (is_dir && n < CAND_MAX - 2) {
        out[n++] = '/';
    }

    out[n] = '\0';
    c->count++;
}

/* an absolute word names a place in the one namespace, so it completes
 * against whatever is mounted there -- the disk at /, the ramdisk at
 * /boot -- without this having to know which is which */
static void gather_path(struct candidates *c, const char *word, size_t wlen) {
    /* split at the last slash: what comes before names the directory to
     * look in, what comes after is the part being matched */
    size_t cut = 0;
    bool have_slash = false;
    for (size_t i = 0; i < wlen; i++) {
        if (word[i] == '/') {
            cut = i;
            have_slash = true;
        }
    }
    if (!have_slash) {
        return;
    }

    char dir[CAND_MAX];
    size_t dlen = (cut == 0) ? 1 : cut;      /* "/" when the slash is first */
    if (dlen >= sizeof dir) {
        return;
    }
    memcpy(dir, word, dlen);
    dir[dlen] = '\0';

    const char *partial = word + cut + 1;
    size_t plen = wlen - cut - 1;

    struct vfs_file e;
    for (size_t i = 0; vfs_readdir(dir, i, &e); i++) {
        size_t n = strlen(e.name);
        if (n < plen || common_prefix(e.name, partial) < plen) {
            continue;
        }
        add_candidate(c, dir, e.name, e.is_dir);
        if (c->count >= MAX_CANDIDATES) {
            break;
        }
    }
}

static void gather(struct candidates *c, const char *line, size_t start,
                   size_t pos, bool first_word) {
    c->count = 0;
    size_t plen = pos - start;
    const char *prefix = line + start;

    if (first_word) {
        for (const struct command *cmd = commands; cmd->name; cmd++) {
            if (strlen(cmd->name) >= plen
                && common_prefix(cmd->name, prefix) >= plen) {
                add_candidate(c, cmd->name, NULL, false);
            }
        }

        /* and the programs, because they are commands too now -- a
         * completion that offered only the builtins would be drawing a
         * line the rest of this version just spent its time rubbing out */
        for (int d = 0; command_path[d] != NULL; d++) {
            struct vfs_file f;
            for (size_t i = 0; vfs_readdir(command_path[d], i, &f); i++) {
                if (f.is_dir || f.name[0] == '\0') {
                    continue;
                }
                size_t n = strlen(f.name);
                if (n < plen || common_prefix(f.name, prefix) < plen) {
                    continue;
                }
                bool already = false;
                for (int k = 0; k < c->count; k++) {
                    if (strcmp(c->items[k], f.name) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    add_candidate(c, f.name, NULL, false);
                }
            }
        }
        return;
    }

    /* offer filenames after a command that takes one, and after any
     * program -- most of them take a filename, and the shell has no way
     * to know which. a builtin that does not is left alone, since
     * completing `echo mo<tab>` into a filename would be surprising */
    const struct command *cmd = command_for_line(line);
    if (cmd != NULL) {
        if (!cmd->takes_file) {
            return;
        }
    } else if (!first_word_is_program(line)) {
        return;
    }

    /* any word with a slash in it names a place in the tree, whether or
     * not it starts at the root -- `boot/mo` is as much a path as
     * `/boot/mo` is */
    bool is_path = false;
    for (size_t i = 0; i < plen; i++) {
        if (prefix[i] == '/') {
            is_path = true;
            break;
        }
    }

    if (is_path) {
        gather_path(c, prefix, plen);
    } else {
        /* a bare name is looked for the way vfs_open looks for one: the
         * disk's root first, then the ramdisk. offered bare, since bare
         * is what was typed */
        struct vfs_file v;
        for (size_t i = 0;
             vfs_readdir("/", i, &v) && c->count < MAX_CANDIDATES; i++) {
            size_t n = strlen(v.name);
            if (n >= plen && common_prefix(v.name, prefix) >= plen) {
                add_candidate(c, v.name, NULL, v.is_dir);
            }
        }
    }

    /* and the ramdisk's own names, which are whole paths carrying no
     * leading slash -- `bin/h` completes to `bin/hello` out of this and
     * out of nothing else */
    struct ramdisk_file f;
    for (size_t i = 0; ramdisk_stat(i, &f) && c->count < MAX_CANDIDATES; i++) {
        const char *name = f.name;
        if (name[0] == '.' && name[1] == '/') {
            name += 2;
        }
        size_t n = strlen(name);
        if (n == 0 || name[n - 1] == '/') {
            continue;       /* tar's directory records have nothing behind them */
        }
        if (n >= plen && common_prefix(name, prefix) >= plen) {
            add_candidate(c, name, NULL, false);
        }
    }
}

static void complete(char *line, size_t *len, size_t *pos) {
    /* the editor does not keep the line terminated while you are typing
     * -- it only does that on enter -- and everything below wants a
     * string. terminate it here, where len is known */
    line[*len] = '\0';

    bool first_word;
    size_t start = word_start(line, *pos, &first_word);

    /* a bare tab in the command position does nothing on purpose:
     * dumping the whole command list is what `help` is for. after a
     * command that takes a filename there is no such list to consult,
     * so an empty word there is worth answering */
    if (*pos == start && first_word) {
        return;
    }

    /* two kilobytes, which is more than the shell thread's stack wants
     * to spare, and the shell is the only thing that completes anything */
    static struct candidates c;
    gather(&c, line, start, *pos, first_word);

    if (c.count == 0) {
        return;
    }
    if (c.count == 1) {
        replace_word(line, len, pos, start, c.items[0]);
        return;
    }

    /* several: fill in as far as they all agree, and only if that adds
     * nothing do I show the list */
    size_t shared = strlen(c.items[0]);
    for (int i = 1; i < c.count; i++) {
        size_t n = common_prefix(c.items[0], c.items[i]);
        if (n < shared) {
            shared = n;
        }
    }
    if (shared > *pos - start) {
        char partial[LINE_MAX];
        for (size_t i = 0; i < shared; i++) {
            partial[i] = c.items[0][i];
        }
        partial[shared] = '\0';
        replace_word(line, len, pos, start, partial);
        return;
    }

    kprintf("\n");
    for (int i = 0; i < c.count; i++) {
        kprintf("  %s", c.items[i]);
    }
    kprintf("\n");
    prompt();
    for (size_t i = 0; i < *len; i++) {
        kprintf("%c", line[i]);
    }
    move_left(*len - *pos);
}

/* read a line for the kernel's own prompts. `echo` off is for a
 * password: the keys still arrive, they just leave no trace on the
 * screen or in the scrollback for the next person to read */
static void read_line(char *buf, size_t max, bool echo) {
    size_t len = 0;
    for (;;) {
        int c = input_getchar_blocking();

        if (c == '\n') {
            kprintf("\n");
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                if (echo) {
                    kprintf("\b \b");
                }
            }
            continue;
        }
        if (c < ' ' || c > '~') {
            continue;
        }
        if (len + 1 < max) {
            buf[len++] = (char)c;
            if (echo) {
                kprintf("%c", (char)c);
            }
        }
    }
    buf[len] = '\0';
}

/* the door. it does not open until somebody names themselves */
static void login(void) {
    char name[AUTH_NAME_MAX];
    char password[AUTH_NAME_MAX];

    for (;;) {
        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("\nname the guest: ");
        console_set_colors(COLOR_TEXT, 0x101018);
        read_line(name, sizeof name, true);

        if (name[0] == '\0') {
            continue;
        }

        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("and the word: ");
        console_set_colors(COLOR_TEXT, 0x101018);
        read_line(password, sizeof password, false);

        int uid = auth_login(name, password);
        if (uid < 0) {
            /* one message for both, because saying which was wrong
             * hands over half of it */
            kprintf("that is not a name and a word i know.\n");
            continue;
        }

        current_uid = uid;
        for (size_t i = 0; i < sizeof current_user; i++) {
            current_user[i] = name[i];
            if (name[i] == '\0') break;
        }

        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("\nwelcome, %s.\n", name);
        if (uid != 0) {
            kprintf("thou art a guest here, and the room knows it.\n");
        }
        console_set_colors(COLOR_TEXT, 0x101018);
        return;
    }
}

static void cmd_logout(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("fare thee well, %s\n", current_user);
    login();
}

void shell_run(void) {
    session_init();

    char line[LINE_MAX];

    console_set_colors(COLOR_TEXT, 0x101018);
    login();

    for (;;) {
        size_t len = 0;     /* characters in the line */
        size_t pos = 0;     /* where the cursor sits within them */
        int hist_pos = hist_count;

        prompt();

        for (;;) {
            int c = input_getchar_blocking();

            if (c == '\n') {
                /* print the tail I was sitting in front of, so the
                 * finished line reads properly before I move on */
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                kprintf("\n");
                break;
            }

            if (c == KEY_CTRL_C) {
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                kprintf("^C\n");
                len = 0;
                cancel_generation++;    /* and call back any personas */
                break;
            }

            /* ---- moving ---- */
            if (c == KEY_LEFT || c == 0x02) {           /* ctrl+b */
                if (pos > 0) { move_left(1); pos--; }
                continue;
            }
            if (c == KEY_RIGHT || c == 0x06) {          /* ctrl+f */
                if (pos < len) { kprintf("%c", line[pos]); pos++; }
                continue;
            }
            if (c == 0x01) {                            /* ctrl+a, home */
                move_left(pos);
                pos = 0;
                continue;
            }
            if (c == 0x05) {                            /* ctrl+e, end */
                while (pos < len) { kprintf("%c", line[pos]); pos++; }
                continue;
            }

            /* ---- history ---- */
            if (c == KEY_UP) {
                if (hist_pos > 0) {
                    hist_pos--;
                    replace_line(line, &len, &pos, history[hist_pos]);
                }
                continue;
            }
            if (c == KEY_DOWN) {
                if (hist_pos < hist_count) {
                    hist_pos++;
                    replace_line(line, &len, &pos,
                                 hist_pos == hist_count ? "" : history[hist_pos]);
                }
                continue;
            }

            /* ---- deleting ---- */
            if (c == '\b') {                            /* backspace */
                if (pos > 0) {
                    for (size_t i = pos - 1; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--; pos--;
                    move_left(1);
                    redraw_tail(line, len, pos);
                }
                continue;
            }
            if (c == KEY_DELETE || c == 0x04) {         /* del, ctrl+d */
                if (pos < len) {
                    for (size_t i = pos; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--;
                    redraw_tail(line, len, pos);
                }
                continue;
            }
            if (c == 0x15) {                            /* ctrl+u, kill line */
                replace_line(line, &len, &pos, "");
                continue;
            }
            if (c == 0x0b) {                            /* ctrl+k, kill to end */
                for (size_t i = pos; i < len; i++) { kprintf(" "); }
                move_left(len - pos);
                len = pos;
                line[len] = '\0';
                continue;
            }
            if (c == 0x17) {                            /* ctrl+w, kill a word */
                size_t start = pos;
                while (start > 0 && !is_word_char(line[start - 1])) start--;
                while (start > 0 && is_word_char(line[start - 1]))  start--;
                size_t removed = pos - start;
                if (removed > 0) {
                    for (size_t i = start; i + removed < len; i++) {
                        line[i] = line[i + removed];
                    }
                    len -= removed;
                    pos = start;
                    move_left(removed);
                    for (size_t i = pos; i < len; i++) kprintf("%c", line[i]);
                    for (size_t i = 0; i < removed; i++) kprintf(" ");
                    move_left(len - pos + removed);
                }
                continue;
            }

            if (c == 0x0c) {                            /* ctrl+l */
                /* wipe the screen and put the prompt back with whatever
                 * was half-typed, cursor where it was. same as every
                 * terminal, and it does not disturb the line */
                console_clear();
                prompt();
                for (size_t i = 0; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                move_left(len - pos);
                continue;
            }

            if (c == '\t') {
                complete(line, &len, &pos);
                continue;
            }

            /* ---- typing ---- */
            if (c < ' ' || c > '~') {
                continue;       /* anything else non-printable is not mine */
            }
            if (len + 1 < LINE_MAX) {
                for (size_t i = len; i > pos; i--) {
                    line[i] = line[i - 1];
                }
                line[pos] = (char)c;
                len++;
                /* print from here to the end, then step back to just
                 * after the character I inserted */
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                pos++;
                move_left(len - pos);
            }
        }

        line[len] = '\0';
        history_add(line);
        run_line(line);
    }
}
