#ifndef SCHED_THREAD_H
#define SCHED_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct addrspace;
struct waitq;

#define THREAD_NAME_MAX  16
#define THREAD_STACK_PAGES 4        /* 16k of kernel stack each, plenty */

enum thread_state {
    THREAD_READY,       /* wants the cpu */
    THREAD_RUNNING,     /* has the cpu */
    THREAD_SLEEPING,    /* waiting for a tick to come around */
    THREAD_BLOCKED,     /* parked on a waitq until somebody says otherwise */
    THREAD_DEAD,        /* finished, waiting to be reaped */
};

struct thread {
    /* the saved stack pointer. everything else about a parked thread
     * lives ON that stack -- this one word is the whole handle */
    uint64_t rsp;

    uint64_t stack_phys;        /* what the pmm gave us, for giving back */
    size_t   stack_pages;

    enum thread_state state;
    uint64_t wake_at;           /* tick to wake on, when SLEEPING */

    void (*entry)(void *);
    void *arg;

    /* ring 3 threads only. the address space owns every page in its
     * lower half -- the program's image and its stack alike -- so
     * there is nothing else to free by hand */
    struct addrspace *space;

    int  id;
    char name[THREAD_NAME_MAX];

    /* the boot thread is a static, everything else came from kmalloc.
     * the reaper needs to know which, or it tries to free a global */
    bool from_heap;

    struct thread *next;        /* circular run queue */

    /* the queue this thread is parked on, and the next one along it.
     * the back-pointer is what lets somebody else take it off that
     * queue -- without it the queue holds a bare pointer to a thread
     * the reaper may free, and killing a blocked thread is a
     * use-after-free waiting to happen */
    struct waitq  *waiting_on;
    struct thread *wait_next;

    /* which program this thread is running, or 0 for a kernel thread.
     * the process outlives the thread, so an exit code survives long
     * enough for a parent to read it */
    int pid;
};

const char *thread_state_name(enum thread_state s);

/* rename a thread in place. exists because the boot thread grows up to
 * become the shell and `ps` should say so */
void thread_set_name(struct thread *t, const char *name);

/* build a thread that will start life inside entry(arg). it lands in the
 * run queue ready to go. returns NULL if memory says no */
struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);

/* hand a dead thread's stack back to the pmm, guard page and all */
void thread_free_stack(struct thread *t);

/* leave, with something to say about how it went. never returns */
void thread_exit(int code) __attribute__((noreturn));

#endif
