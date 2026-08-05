#ifndef SCHED_SCHED_H
#define SCHED_SCHED_H

#include <stdint.h>
#include "sched/thread.h"

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

/* walk the run queue (for the `ps` command in m6) */
void sched_dump(void);

#endif
