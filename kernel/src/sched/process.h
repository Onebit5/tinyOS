#ifndef SCHED_PROCESS_H
#define SCHED_PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/* how many files one process may hold open. 0, 1 and 2 are spoken for
 * by the console, the way they are everywhere, so an opened file gets
 * the first number from 3 up */
#define MAX_FDS         8
#define FD_STDIN        0
#define FD_STDOUT       1
#define FD_STDERR       2
#define FD_FIRST_FILE   3

/* what we record when a process was killed rather than choosing to go */
#define PROCESS_KILLED  (-1)

/* a file, as far as a process is concerned: somewhere in the ramdisk
 * and how far through it we are. nothing is copied -- the archive is
 * already in memory and read-only, so a descriptor is a bookmark */
struct fd {
    bool            open;
    const uint8_t  *data;
    uint64_t        size;
    uint64_t        pos;
};

struct process {
    int      pid;               /* 0 means the slot is free */
    int      parent;            /* pid of whoever started it, 0 for the shell */
    int      uid;               /* who it runs as. 0 is the master */
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
};

/* claim a slot. returns the new pid, or 0 if the table is full */
int  process_create(const char *name, int parent, int uid, uint64_t now_ms);

/* who a process runs as. -1 if there is no such pid, which callers
 * treat as "not allowed" rather than "allowed" */
int  process_uid(int pid);

/* note which thread is running it, once there is one */
void process_set_thread(int pid, int thread_id);

/* it finished. the slot stays occupied, holding the code, until
 * somebody collects it */
void process_exited(int pid, int code, uint64_t now_ms);

/* has it finished? if so, take the code and free the slot. returns
 * false while it is still running, or if there is no such pid */
bool process_collect(int pid, int *code);

const struct process *process_find(int pid);

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

/* where the descriptor has got to, and how much is left. false if the
 * fd was never opened */
bool process_fd_peek(int pid, int fd, const void **data, uint64_t *remaining);

/* note that n bytes were taken */
void process_fd_advance(int pid, int fd, uint64_t n);

bool process_fd_close(int pid, int fd);

/* how many a process is holding, for `ps` and the tests */
size_t process_fd_count(int pid);

/* walk the table. index from 0; slots that are free are skipped */
const struct process *process_at(size_t index);
size_t process_count(void);

#endif
