#ifndef FS_PIPE_H
#define FS_PIPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "sched/sched.h"        /* for struct waitq */
#include "sched/spinlock.h"

/* a pipe: a buffer with a process at each end.
 *
 * `cat x | head` has been an error message since 0.1.10, and the reason
 * is that there was nothing for the arrow to be made of. a pipe is not
 * much -- a ring buffer, a count of who still holds each end, and two
 * places to sleep -- but the two rules hanging off those counts are the
 * whole of why pipelines work at all:
 *
 *   a read with nothing in the buffer blocks, *unless* every writer has
 *   gone, in which case it returns 0. that zero is end of file, and it
 *   is the only reason `head` ever stops rather than waiting forever
 *   for a `cat` that finished minutes ago.
 *
 *   a write with no reader left is pointless by definition -- nobody
 *   will ever collect it -- so it fails rather than filling a buffer
 *   that will never drain. unix raises SIGPIPE and the default is to
 *   die; I have no signals, so the process is ended for it, which is
 *   the same outcome by a shorter road. it is what makes `cat huge |
 *   head` stop instead of blocking forever once head has seen enough.
 *
 * the buffer is one page. that number is also the promise: a write of
 * PIPE_BUF or less either fits or waits until it fits, so it never
 * arrives interleaved with somebody else's. bigger writes are split,
 * and always were, on every system that has ever had pipes. */

#define PIPE_BUF 4096

struct pipe {
    struct spinlock lock;

    uint8_t  data[PIPE_BUF];
    uint32_t head;              /* where the next byte comes out */
    uint32_t count;             /* how many are in there */

    /* how many descriptors still hold each end. a pipe frees itself
     * when both reach zero, which is what makes lifetime a question
     * nobody above has to think about */
    int      readers;
    int      writers;

    struct waitq readable;      /* writers wake this */
    struct waitq writable;      /* readers wake this */
};

/* ---- the part with no scheduler in it ----
 *
 * split out on purpose: a ring buffer is exactly the kind of thing that
 * is wrong at the wrap and right everywhere else, and that is only
 * findable by a test that can run it a hundred thousand times. these
 * take no locks and never block, so the host suite can do that */

uint32_t pipe_pending(const struct pipe *p);    /* bytes waiting */
uint32_t pipe_room(const struct pipe *p);       /* bytes that would fit */

/* move what fits / what is there. neither ever blocks, and both return
 * how many bytes actually moved, which may be zero */
uint32_t pipe_put(struct pipe *p, const void *buf, uint32_t len);
uint32_t pipe_get(struct pipe *p, void *buf, uint32_t len);

/* set one up by hand, for tests and for pipe_create */
void pipe_reset(struct pipe *p);

/* ---- the part that blocks ---- */

/* a new pipe, held open at both ends. NULL if there is no memory */
struct pipe *pipe_create(void);

/* let go of one end. the pipe frees itself once nobody holds either,
 * and whoever is left at the other end is woken to find out */
void pipe_close_read(struct pipe *p);
void pipe_close_write(struct pipe *p);

/* one more holder of an end. this is what fork does about a pipe: the
 * child gets the same pipe rather than a copy of it, and the count has
 * to say so -- otherwise the parent closing its end tells the far side
 * there is nobody left when there plainly is, and the far side stops
 * reading a pipe that is still being written to */
void pipe_share(struct pipe *p, bool writing);

/* read, blocking until there is something or every writer has gone.
 * returns 0 at end of file, -1 if `pid` was interrupted while waiting */
int64_t pipe_read(struct pipe *p, int pid, void *buf, uint64_t len);

/* write, blocking until it fits. returns -1 if there is no reader left,
 * which the caller is expected to treat as fatal to the writer */
int64_t pipe_write(struct pipe *p, int pid, const void *buf, uint64_t len);

/* let go of whichever ends a process was holding.
 *
 * idempotent on purpose, and it has to be: a process stops running by
 * one of two roads, and only one of them goes through thread_exit. a
 * program that chooses to leave runs that; one that is killed never
 * does -- sched_kill marks the thread dead and it simply never executes
 * again. so the reaper calls this too, and so does the sweep that
 * collects abandoned processes, and whichever gets there first is the
 * one that counts.
 *
 * it matters more than tidiness. a stage of a pipeline that dies
 * without releasing its ends leaves the stage after it asleep on a pipe
 * that will never say end of file -- which looks exactly like a hung
 * machine and is not */
void pipe_release_for(int pid);

/* how many exist right now, for `ps` and for finding a leak */
size_t pipe_count(void);

#endif
