#include "sched/thread.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "cpu/interrupts.h"

static int next_id = 1;     /* 0 belongs to the boot thread */

const char *thread_state_name(enum thread_state s) {
    switch (s) {
    case THREAD_READY:    return "ready";
    case THREAD_RUNNING:  return "running";
    case THREAD_SLEEPING: return "sleeping";
    case THREAD_BLOCKED:  return "blocked";
    case THREAD_DEAD:     return "dead";
    default:              return "???";
    }
}

void thread_set_name(struct thread *t, const char *name) {
    size_t n = 0;
    while (name[n] && n < THREAD_NAME_MAX - 1) {
        t->name[n] = name[n];
        n++;
    }
    t->name[n] = '\0';
}

/* where every new thread opens its eyes. we arrive here by `ret` out of
 * switch_context, not by iretq, which has one important consequence
 * spelled out below */
static void thread_bootstrap(void) {
    /* we inherited IF=0 from whoever switched to us, because switching
     * happens with interrupts off. a preempted thread would get its
     * flags back from the iretq it eventually returns through, and a
     * yielding one from irq_restore -- but we have no such history to
     * return through. so we let interrupts back in ourselves.
     * forget this line and the first thread you spawn quietly kills
     * preemption for the whole system */
    asm volatile ("sti");

    struct thread *me = sched_current();
    me->entry(me->arg);
    thread_exit();
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg) {
    struct thread *t = kmalloc(sizeof *t);
    if (t == NULL) {
        return NULL;
    }

    uint64_t phys = pmm_alloc_pages(THREAD_STACK_PAGES);
    if (phys == 0) {
        kfree(t);
        return NULL;
    }

    t->stack_phys  = phys;
    t->stack_pages = THREAD_STACK_PAGES;
    t->state       = THREAD_READY;
    t->wake_at     = 0;
    t->entry       = entry;
    t->arg         = arg;
    t->id          = next_id++;
    t->next        = NULL;
    t->wait_next   = NULL;

    thread_set_name(t, name);

    /* fabricate a stack that looks exactly like a thread which is
     * sitting inside switch_context waiting to be resumed. the pops
     * over there will eat our six zeroes, and its `ret` will land on
     * thread_bootstrap. stack top is page aligned, so the return
     * address slot ends up 16-aligned and bootstrap gets the stack
     * alignment the abi promises it */
    uint8_t *stack = pmm_phys_to_virt(phys);
    uint64_t *sp = (uint64_t *)(stack + THREAD_STACK_PAGES * PAGE_SIZE);

    *--sp = 0;                              /* bootstrap never returns, but if
                                             * it somehow did, land on 0 loudly */
    *--sp = (uint64_t)thread_bootstrap;     /* switch_context's ret target */
    *--sp = 0;                              /* rbp */
    *--sp = 0;                              /* rbx */
    *--sp = 0;                              /* r12 */
    *--sp = 0;                              /* r13 */
    *--sp = 0;                              /* r14 */
    *--sp = 0;                              /* r15 */

    t->rsp = (uint64_t)sp;

    sched_add(t);
    return t;
}

void thread_exit(void) {
    struct thread *me = sched_current();

    kprintf("[%s] hath returned to the sea of souls\n", me->name);

    uint64_t flags = irq_save();
    me->state = THREAD_DEAD;
    irq_restore(flags);

    /* the scheduler will never pick a dead thread, so this yield is a
     * one way door. the next thread to run reaps our stack out from
     * under us, which is only safe because we are never coming back */
    for (;;) {
        sched_yield();
    }
}
