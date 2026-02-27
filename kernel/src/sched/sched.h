#ifndef SCHED_SCHED_H
#define SCHED_SCHED_H

#include <stdint.h>
#include "sched/thread.h"
#include <stddef.h>
#include <stdbool.h>

/* round robin preemptive scheduler. one core, one run queue, no
 * priorities, no fairness accounting. it takes turns, thats it */

/* adopts whatever is currently executing as thread 0 and spawns the
 * idle thread. after this, kmain IS a thread */
void sched_init(void);

/* give up the rest of the timeslice */
void sched_yield(void);

/* block for a while. the cpu goes to somebody who can use it */
void sleep_ms(uint64_t ms);

/* called from the timer irq. counts down the quantum and preempts */
void sched_tick(void);

struct thread *sched_current(void);

/* drop a freshly built thread into the run queue. thread_create calls
 * this for you, you probably want that instead */
void sched_add(struct thread *t);

/* a place for threads to wait for something that isnt a clock.
 * the keyboard uses one so the shell can sleep until you press a key
 * instead of spinning asking "any keys yet? any keys yet?" */
struct waitq {
    struct thread *head;
};

/* park the running thread until somebody wakes this queue.
 * MUST be entered with interrupts off, and returns with them still off
 * -- thats what makes "check the buffer, then sleep" atomic. without
 * that, a key landing between the check and the sleep is lost and the
 * thread waits forever for something that already happened */
void waitq_block(struct waitq *q);

/* the same thing in two halves, for a caller holding a lock of its own.
 * go on the queue first, then drop your lock, then sleep -- a wake that
 * lands in the gap has already marked you ready, so the sleep returns
 * at once instead of being missed.
 *
 * a lock must never be held across the sleep. turning interrupts off is
 * a property of a thread and rides through a context switch harmlessly;
 * a lock is a property of the machine, and a sleeping thread holding one
 * is a machine where nobody else can ever have it */
void waitq_enqueue(struct waitq *q);
void waitq_sleep(void);

/* given back by a thread on its very first run, because it starts
 * holding a lock whose release is on a stack it will never return to */
void sched_first_run(void);

/* a core other than the first, joining the scheduler. called on the core
 * that will run it, after it has its own descriptor tables */
bool sched_join(unsigned cpu, const char *idle_name);

/* which core a thread is on, or -1 if it is not running anywhere */
int sched_thread_cpu(const struct thread *t);

/* how many cores are actually taking work */
size_t sched_cores_scheduling(void);

/* the name of whatever a given core is running this instant, for `cpus` */
const char *sched_cpu_running(unsigned cpu);

/* wake everyone parked on the queue. safe to call from an irq */
void waitq_wake_all(struct waitq *q);

/* walk the run queue (for the `ps` command in m6) */
void sched_dump(void);

/* how many threads are in the ring, dead ones included */
size_t sched_thread_count(void);

/* is there still a thread with this id that has not finished? asking by
 * id rather than by pointer on purpose -- the reaper may free the
 * struct at any moment, and an id cannot dangle */
bool sched_thread_alive(int id);

enum sched_kill_result {
    SCHED_KILL_OK,
    SCHED_KILL_NO_SUCH,
    SCHED_KILL_SELF,        /* the caller asked to end itself */
    SCHED_KILL_PROTECTED,   /* idle -- somebody has to take the cpu */
};

/* mark a thread dead so the reaper collects it. a thread parked on a
 * waitq is taken off it first, which is the whole reason threads now
 * carry a pointer to the queue they are waiting on */
enum sched_kill_result sched_kill(int id);

/* make a thread runnable wherever it is: asleep, or parked on a queue.
 * used to deliver an interrupt, which has to reach a process that is
 * not currently asking for anything */
void sched_wake_thread(int id);

/* take one thread off a queue without waking it. used when a thread is
 * killed while blocked -- the queue must not be left holding a pointer
 * to something the reaper is about to free */
void waitq_remove(struct waitq *q, struct thread *t);

#endif
