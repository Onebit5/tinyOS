#ifndef SCHED_SPINLOCK_H
#define SCHED_SPINLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* locks, at last.
 *
 * this kernel has spent twelve versions using `cli` as mutual exclusion.
 * that worked, and it worked for a real reason: with one core, the only
 * thing that could interrupt a critical section was an interrupt, and
 * turning them off stopped it. it is not a shortcut, it is the correct
 * answer to the question that was being asked.
 *
 * the question changed in 0.2.0. turning interrupts off on *this* core
 * says nothing whatsoever about a thread running on *that* one. every
 * one of those thirty-nine places was quietly reclassified from correct
 * to wrong by a version that did not touch any of them.
 *
 * so: a real lock, and both halves of the problem at once. a critical
 * section usually needs protecting from two different things -- this
 * core's own interrupt handlers, and the other cores -- and needs both
 * or neither. spin_lock_irq does both and hands back the flags, so it
 * drops into exactly the shape irq_save/irq_restore already had. that
 * was deliberate: an audit of thirty-nine call sites is worth doing
 * where every change looks the same and any that does not stands out.
 *
 * ---- the order they must be taken in ----
 *
 * two locks taken in opposite orders by two cores is a machine that
 * stops, with no fault and nothing printed. the only defence is a rank
 * every lock declares and never violates: a lock may only be taken
 * while holding locks of *lower* rank. it is checked, not hoped for. */

/* the order is not invented -- it is read off the call graph. the tty
 * calls the scheduler, the scheduler calls the process table, and every
 * one of them may allocate; nothing calls back the other way. anything
 * may print, and printing calls nobody */
enum lock_rank {
    /* a pipe is the lowest thing there is: it wakes threads and does
     * nothing else at all, so it may be held while reaching up to the
     * scheduler and there is nothing beneath it to reach down to */
    LOCK_RANK_PIPE = 1,
    LOCK_RANK_DEVICE,           /* tty, input, rtc, pci, the disk */
    LOCK_RANK_SCHED,            /* the run queue */
    LOCK_RANK_PROCESS,          /* the process table, which sched reaches into */
    LOCK_RANK_HEAP,             /* slab, and kmalloc above it */
    LOCK_RANK_PMM,              /* which the heap calls into, never the reverse */
    LOCK_RANK_PRINT,            /* anything may print; printing takes nothing */
};

struct spinlock {
    volatile uint32_t held;
    const char *name;
    enum lock_rank rank;

    /* which core has it, for saying something useful when it goes wrong.
     * ~0 when free */
    volatile uint32_t owner;

    /* how often somebody had to wait. cheap, and the only way to know
     * whether a lock is the one worth splitting later */
    volatile uint64_t contended;

    bool warned;                /* an out-of-order take, reported once */

    /* whether this one is in the list the shell shows. a lock declared
     * where it is defined never calls spin_init, so it puts itself on
     * the list the first time it is taken -- otherwise the only locks
     * anybody could see would be the ones set up by hand, which is none
     * of them */
    volatile uint32_t listed;
};

#define SPINLOCK(nm, rk) { 0, nm, rk, ~0u, 0, false, 0 }

void spin_init(struct spinlock *l, const char *name, enum lock_rank rank);

/* the workhorse: interrupts off *and* the lock held, because a section
 * that needs one almost always needs the other. returns the flags to
 * hand back */
uint64_t spin_lock_irq(struct spinlock *l);
void spin_unlock_irq(struct spinlock *l, uint64_t flags);

/* when interrupts are already off, or cannot matter */
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* once the machine is dying, locks are in the way rather than any use:
 * a panic while holding one would take the print lock, find it already
 * held by this very core, and panic about that instead. called first
 * thing by panic() */
void spin_abandon_all(void);

/* for the shell, and for deciding what to split up later */
size_t spin_count(void);
const struct spinlock *spin_at(size_t index);

#endif
