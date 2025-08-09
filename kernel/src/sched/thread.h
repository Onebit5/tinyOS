#ifndef SCHED_THREAD_H
#define SCHED_THREAD_H

#include <stdint.h>
#include <stddef.h>

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

    int  id;
    char name[THREAD_NAME_MAX];

    struct thread *next;        /* circular run queue */
    struct thread *wait_next;   /* the waitq we're parked on, if any */
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

/* leave. never returns, obviously */
void thread_exit(void) __attribute__((noreturn));

#endif
