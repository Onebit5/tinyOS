#ifndef SCHED_PROCESS_H
#define SCHED_PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/path.h"

struct pipe;

/* a process is a program someone started, and it outlives the thread
 * that ran it.
 *
 * that outliving is the entire point. a thread is reaped the moment it
 * dies -- its stack and address space handed straight back -- so if the
 * exit code lived on the thread there would be nothing left to read it
 * from by the time a parent got round to asking. so the code, the name
 * and the parentage live here instead, in a slot that stays occupied
 * until somebody collects it. a process that has ended but not been
 * collected is what everyone else calls a zombie. */

#define MAX_PROCESSES   32
#define PROC_NAME_MAX   24

/* how much environment one process may carry. a kilobyte is about
 * thirty ordinary variables, and this is thirty-two processes */
#define PROC_ENV_MAX    1024
#define ENV_NAME_MAX    64
#define ENV_VALUE_MAX   256

/* how many files one process may hold open. 0, 1 and 2 are spoken for
 * the way they are everywhere, so an opened file gets the first number
 * from 3 up */
#define MAX_FDS         12
#define FD_STDIN        0
#define FD_STDOUT       1
#define FD_STDERR       2
#define FD_FIRST_FILE   3

/* what I record when a process was killed rather than choosing to go */
#define PROCESS_KILLED  (-1)

/* init.
 *
 * pid 1 is a convention everywhere and it is a convention for a reason:
 * reparenting needs a number that is known before the process it names
 * exists. this table hands out pids in order from 1 and init is the
 * first thing in it, so the two agree by construction rather than by
 * anybody remembering to keep them in step.
 *
 * pid *0* is the other special number and is not a process at all: it is
 * what the kernel shell uses, since a shell here is a kernel thread. the
 * difference matters to exactly one rule, and it is the important one --
 * a process whose parent is 0 belongs to a shell that waits for its own
 * and reads their exit codes, so init must not collect it */
#define INIT_PID        1

/* what a descriptor can be pointing at.
 *
 * until 0.2.9, 0, 1 and 2 were not descriptors at all -- the syscall
 * layer answered them directly, because until pipes arrived there was
 * exactly one place each of them could point. that was fine right up
 * until `echo hi > file.txt`, which is stdout pointing at a *file*, and
 * there was nowhere to write that down.
 *
 * so they are real slots now, filled in when the process is made. every
 * read and every write is one lookup and a switch, and redirection is
 * just a different thing in the slot before the program starts. it is
 * the arrangement unix has had since the beginning and the reason is
 * exactly this: a program that cannot tell where its output goes is a
 * program that works in a pipeline, in a file, and on a screen without
 * knowing which. */
enum fd_kind {
    FD_FREE = 0,
    FD_CONSOLE,     /* the screen. writes print; reads are meaningless */
    FD_KEYBOARD,    /* the line discipline. reads a line; writes are not */
    FD_MEMORY,      /* a file already in memory, which is the ramdisk */
    FD_DISK,        /* a file out on the disk, fetched as it is asked for */
    FD_PIPE,
};

struct fd {
    enum fd_kind    kind;

    /* a file in the ramdisk is already in memory, so the descriptor is
     * a bookmark. one on the disk is not, so all it can remember is
     * where the file starts -- the bytes get fetched on demand */
    const uint8_t  *data;       /* in memory */

    /* on disk: where the file starts, and where the record describing
     * it lives, so a write can correct the size afterwards */
    uint32_t        cluster;
    uint64_t        entry_sector;
    uint32_t        entry_offset;

    uint64_t        size;
    uint64_t        pos;

    struct pipe    *pipe;
    /* which end of a pipe this is, and for a file whether it was opened
     * to be written to. one bit, because a descriptor here goes one way
     * -- there is no read-write open and nothing has wanted one */
    bool            writing;
};

/* everything a disk-backed descriptor knows about its file */
struct fd_disk {
    uint32_t cluster;
    uint64_t size;
    uint64_t pos;
    uint64_t remaining;
    uint64_t entry_sector;
    uint32_t entry_offset;
};

struct process {
    int      pid;               /* 0 means the slot is free */
    int      parent;            /* pid of whoever started it, 0 for the shell */

    /* which job this belongs to.
     *
     * `cat x | grep y | wc -l` is three processes and *one* thing the
     * person typing it is thinking about. ctrl+z has to stop all three
     * or none, ctrl+c has to reach all three, and `fg` has to bring all
     * three back -- so they share a number, and that number is the pid
     * of the first of them. a command on its own is a group of one */
    int      pgid;
    int      uid;               /* who it runs as. 0 is the master */

    /* whether to narrate this one's comings and goings. `run bin/hello`
     * is a demonstration and the ceremony is the point; `cat motd.txt`
     * is somebody trying to read a file, and a line about souls
     * returning to the sea is just noise on top of the answer */
    bool     announce;
    int      thread_id;         /* the thread running it, while it lives */
    char     name[PROC_NAME_MAX];
    bool     exited;
    int      exit_code;

    /* an interrupt has been delivered and not yet looked at. this is as
     * close to a signal as this kernel gets: a flag the process finds
     * the next time it asks the kernel for anything */
    bool     interrupted;
    uint64_t started_ms;
    uint64_t ended_ms;
    struct fd fds[MAX_FDS];

    /* ---- the environment ------------------------------------------
     *
     * one block of "NAME=value" strings, each ended by a NUL, with an
     * empty string for the end of the lot. that shape is not nostalgia:
     * it is what makes the whole thing one memcpy to inherit, and
     * inheriting is most of what an environment is *for*. a table of
     * pointers would need every one of them rewritten on the way into a
     * child.
     *
     * it belongs to the process rather than to the program, which is
     * why `export` in every shell there has ever been is a builtin and
     * not a command -- a command could only ever change its own */
    char env[PROC_ENV_MAX];
    size_t env_len;

    /* where this process is standing. every relative name it uses is
     * read from here, and it inherits whatever its parent was in --
     * which is what makes `cd` somewhere and then running something
     * mean what anybody would expect */
    char cwd[PATH_MAX];
};

/* claim a slot. returns the new pid, or 0 if the table is full */
/* where a process is standing, and moving it. an unknown pid is at the
 * root, which is what the shell's own lookups want before anybody has
 * said otherwise */
const char *process_cwd(int pid);
void        process_set_cwd(int pid, const char *path);

int  process_create(const char *name, int parent, int uid, bool announce,
                    uint64_t now_ms);

/* should this one's arrival and departure be narrated? */
bool process_announces(int pid);

/* who a process runs as. -1 if there is no such pid, which callers
 * treat as "not allowed" rather than "allowed" */
int  process_uid(int pid);

/* ---- groups ---------------------------------------------------------
 * a job is a group, and everything the terminal does it does to a whole
 * one: the keys belong to a group, ctrl+c reaches a group, ctrl+z stops
 * a group */

int  process_pgid(int pid);
void process_set_pgid(int pid, int pgid);

/* the thread ids of everything still running in a group. that is what
 * stopping and continuing need, and it is gathered under the table's
 * lock and acted on afterwards -- the scheduler may not be reached
 * while this lock is held */
size_t process_group_threads(int pgid, int *ids, size_t max);

/* deliver an interrupt to every member. one ctrl+c, the whole job */
void process_interrupt_group(int pgid);

/* is anything in the group still going? a job whose last member has
 * ended is a job that is over */
bool process_group_alive(int pgid);

/* note which thread is running it, once there is one */
void process_set_thread(int pid, int thread_id);

/* it finished. the slot stays occupied, holding the code, until
 * somebody collects it */
void process_exited(int pid, int code, uint64_t now_ms);

/* has it finished? if so, take the code and free the slot. returns
 * false while it is still running, or if there is no such pid */
bool process_collect(int pid, int *code);

/* ---- what init owns --------------------------------------------------
 *
 * a process that has ended and not been collected is holding a slot, and
 * there are thirty-two of them. normally its parent collects it -- that
 * is what `wait` is -- but a parent can die first, and then the exit code
 * is addressed to nobody and the slot is held forever.
 *
 * so children are reparented to init as their parent goes (that happens
 * inside process_exited, since the moment the parent ends is the only
 * moment anybody could notice), and init collects them. */

/* the pid of something init should collect, or 0 when there is nothing.
 *
 * "should" is narrow on purpose. this returns only what is genuinely
 * nobody's: adopted by init when its parent died, or the child of a
 * parent that has left the table entirely. a process whose parent is 0
 * is a kernel shell's, and taking its exit code out of the shell's hand
 * would be a `$?` that is sometimes right */
int process_orphan(void);

/* raise the interrupt flag on everything still running, and say how many
 * that was. init's shutdown asks twice: once to ask, and again after a
 * grace period to find out whether asking worked */
size_t process_interrupt_all(void);

/* the thread ids of every process still going, for a shutdown that has
 * run out of patience. gathered under the lock and acted on afterwards,
 * because killing a thread reaches the scheduler and a holder of this
 * lock may not */
size_t process_running_threads(int *ids, size_t max);

const struct process *process_find(int pid);

/* what it is called. a forked child takes its parent's name, since it
 * is the same program */
const char *process_name(int pid);

/* ---- the environment ------------------------------------------------
 *
 * these work on the block rather than on one variable at a time,
 * because the block is what gets inherited and inheriting is most of
 * what an environment is for */

/* replace the whole environment. `len` counts the trailing empty string */
bool process_set_env(int pid, const char *block, size_t len);

/* a copy of it. returns how many bytes, including the terminator */
size_t process_get_env(int pid, char *out, size_t max);

/* look one variable up. false if it is not set -- which is a different
 * answer from being set to nothing, and both are worth being able to
 * give */
bool process_env_get(int pid, const char *name, char *out, size_t max);

/* set or replace one. a NULL value removes it */
bool process_env_set(int pid, const char *name, const char *value);

/* ---- the same operations on a bare block ----
 *
 * split out because the shell is a kernel thread with no process entry
 * of its own, and its environment therefore lives in its session. one
 * implementation, two callers, and no chance of them disagreeing about
 * what "already set" means */
bool env_block_get(const char *block, size_t len, const char *name,
                   char *out, size_t max);
bool env_block_set(char *block, size_t *len, size_t max, const char *name,
                   const char *value);

/* deliver an interrupt. the process finds it on its next syscall */
void process_interrupt(int pid);

/* is one waiting, unlooked-at? asking does not consume it */
bool process_interrupt_pending(int pid);

/* take it, clearing the flag. true if there was one */
bool process_take_interrupt(int pid);

/* ---- open files -----------------------------------------------------
 * the process owns these, so they close themselves when it ends. the
 * copying in and out is left to the syscall layer, which is the only
 * place that knows how to check a pointer ring 3 handed over */

/* take a descriptor onto a stretch of ramdisk. returns the fd, or -1
 * if this process is already holding as many as it may */
int  process_fd_open(int pid, const void *data, uint64_t size);

/* the same, for a file whose bytes are still on the disk */
int  process_fd_open_disk(int pid, uint32_t cluster, uint64_t size,
                          uint64_t entry_sector, uint32_t entry_offset);

/* where a disk-backed descriptor has got to. false for a memory one */
bool process_fd_disk(int pid, int fd, struct fd_disk *out);

/* a write may have grown the file, or given an empty one its first
 * cluster. the descriptor has to learn about both */
void process_fd_grew(int pid, int fd, uint32_t cluster, uint64_t size);

/* where the descriptor has got to, and how much is left. false if the
 * fd was never opened */
bool process_fd_peek(int pid, int fd, const void **data, uint64_t *remaining);

/* note that n bytes were taken */
void process_fd_advance(int pid, int fd, uint64_t n);

/* a copy of what a descriptor points at. a copy rather than a pointer
 * on purpose: the caller is about to do things that block -- read a
 * pipe, fetch a sector -- and none of that may happen while holding the
 * table this lives in */
bool process_fd_get(int pid, int fd, struct fd *out);

/* put something in a numbered slot. this is how redirection happens:
 * before a program starts, whoever is wiring it up puts a pipe or a
 * file where the console would have been, and the program never finds
 * out */
bool process_fd_install(int pid, int fd, const struct fd *src);

/* take every one of `from`'s descriptors, pointing at the same things.
 * this is what fork does about open files, and the interesting part is
 * the pipes: one gains a holder rather than being duplicated, or the
 * parent closing its end would tell the far side there is nobody left
 * when there plainly is */
bool process_fds_inherit(int pid, int from);

/* ---- the ends of a pipeline ---------------------------------------- */

struct pipe_end {
    struct pipe *p;
    bool         writing;
};

/* take every pipe this process holds off it and hand them back, so the
 * caller can close them without holding the process table's lock while
 * it does. that matters: closing a pipe wakes threads, and waking
 * threads means reaching the scheduler, which a holder of this lock may
 * not do. returns how many were taken */
size_t process_take_pipes(int pid, struct pipe_end *out, size_t max);

/* close one. a pipe in that slot is handed back through `closing`
 * rather than let go of here, for the same reason as everything else on
 * this page. `closing->p` comes back NULL when it was not a pipe */
bool process_fd_close(int pid, int fd, struct pipe_end *closing);

/* how many a process is holding, for `ps` and the tests */
size_t process_fd_count(int pid);

/* walk the table. index from 0; slots that are free are skipped */
const struct process *process_at(size_t index);
size_t process_count(void);

#endif
