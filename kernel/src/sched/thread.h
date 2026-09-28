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
     * lives ON that stack -- this one word is the whole handle.
     *
     * called `rsp` until 0.2.21, which is the x86 name for it and was a
     * register named in portable code. what it is, is the stack pointer */
    uint64_t sp;

    uint64_t stack_phys;        /* what the pmm gave me, for giving back */
    size_t   stack_pages;

    enum thread_state state;

    /* suspended by ctrl+z, and not to be picked until somebody says
     * otherwise.
     *
     * a flag rather than a state, and that is the whole trick. a thread
     * that is stopped may *also* be blocked on a pipe, or asleep, or
     * ready -- those are answers to "what is it waiting for" and this
     * is an answer to "may it run at all". squeezing both into one enum
     * would mean a stopped-then-woken thread forgetting it was stopped,
     * which is a program that resumes itself the moment anybody types
     * at it */
    bool     stopped;

    uint64_t wake_at;           /* tick to wake on, when SLEEPING */

    /* how many timer ticks this thread was the one running when the
     * timer went off. it is a sampling measure rather than a real
     * accounting -- a thread that always yields just before the tick
     * would look free -- but it is honest about being one, and it is
     * what turns the scheduler from a claim into something you can
     * watch */
    uint64_t cpu_ticks;

    void (*entry)(void *);
    void *arg;

    /* ring 3 threads only. the address space owns every page in its
     * lower half -- the program's image and its stack alike -- so
     * there is nothing else to free by hand */
    struct addrspace *space;

    int  id;

    /* which screen this thread's output goes to, and which keyboard it
     * may read. inherited from whoever created it, so a program started
     * from the shell on console 2 prints on console 2 -- output belongs
     * to its writer rather than to whichever console is being looked at,
     * and that is the whole difference between four consoles and one
     * console with four names */
    unsigned console;

    char name[THREAD_NAME_MAX];

    /* which core this is on, or -1 for none. a thread in the ring marked
     * RUNNING is on somebody's cpu right now, and no other core may pick
     * it up -- two cores running the same thread would be two cores on
     * one stack, which ends exactly as badly as it sounds */
    int  on_cpu;

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

/* the same, but parked: it goes into the ring already blocked, so no
 * other core can pick it up before its own has claimed it. a core
 * building its idle thread needs exactly this -- between creating one
 * and saying "this is mine", a ready thread is fair game to anybody */
struct thread *thread_create_parked(const char *name, void (*entry)(void *),
                                    void *arg);

/* give a dead thread's struct back. does nothing for the boot thread,
 * which was never allocated in the first place */
void thread_free(struct thread *t);

/* hand a dead thread's stack back to the pmm, guard page and all */
void thread_free_stack(struct thread *t);

/* leave, with something to say about how it went. never returns */
void thread_exit(int code) __attribute__((noreturn));

#endif
