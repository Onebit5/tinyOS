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

/* what we record when a process was killed rather than choosing to go */
#define PROCESS_KILLED  (-1)

struct process {
    int      pid;               /* 0 means the slot is free */
    int      parent;            /* pid of whoever started it, 0 for the shell */
    int      thread_id;         /* the thread running it, while it lives */
    char     name[PROC_NAME_MAX];
    bool     exited;
    int      exit_code;
    uint64_t started_ms;
    uint64_t ended_ms;
};

/* claim a slot. returns the new pid, or 0 if the table is full */
int  process_create(const char *name, int parent, uint64_t now_ms);

/* note which thread is running it, once there is one */
void process_set_thread(int pid, int thread_id);

/* it finished. the slot stays occupied, holding the code, until
 * somebody collects it */
void process_exited(int pid, int code, uint64_t now_ms);

/* has it finished? if so, take the code and free the slot. returns
 * false while it is still running, or if there is no such pid */
bool process_collect(int pid, int *code);

const struct process *process_find(int pid);

/* walk the table. index from 0; slots that are free are skipped */
const struct process *process_at(size_t index);
size_t process_count(void);

#endif
