#ifndef SCHED_PROCESS_H
#define SCHED_PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/path.h"

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

/* what I record when a process was killed rather than choosing to go */
#define PROCESS_KILLED  (-1)

/* a file, as far as a process is concerned: somewhere in the ramdisk
 * and how far through it I am. nothing is copied -- the archive is
 * already in memory and read-only, so a descriptor is a bookmark */
struct fd {
    bool            open;

    /* a file in the ramdisk is already in memory, so the descriptor is
     * a bookmark. a file on the disk is not, so all it can remember is
     * where the file starts -- the bytes get fetched on demand */
    bool            on_disk;
    const uint8_t  *data;       /* in memory */

    /* on disk: where the file starts, and where the record describing
     * it lives, so a write can correct the size afterwards */
    uint32_t        cluster;
    uint64_t        entry_sector;
    uint32_t        entry_offset;

    uint64_t        size;
    uint64_t        pos;
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

bool process_fd_close(int pid, int fd);

/* how many a process is holding, for `ps` and the tests */
size_t process_fd_count(int pid);

/* walk the table. index from 0; slots that are free are skipped */
const struct process *process_at(size_t index);
size_t process_count(void);

#endif
