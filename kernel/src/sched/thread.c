#include "sched/thread.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/addrspace.h"
#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "cpu/interrupts.h"
#include "sched/process.h"
#include "drivers/pit.h"

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

/* where every new thread opens its eyes. I arrive here by `ret` out of
 * switch_context, not by iretq, which has one important consequence
 * spelled out below */
static void thread_bootstrap(void) {
    /* I inherited IF=0 from whoever switched to me, because switching
     * happens with interrupts off. a preempted thread would get its
     * flags back from the iretq it eventually returns through, and a
     * yielding one from irq_restore -- but I have no such history to
     * return through. so I let interrupts back in myself.
     * forget this line and the first thread you spawn quietly kills
     * preemption for the whole system */
    asm volatile ("sti");

    struct thread *me = sched_current();
    me->entry(me->arg);
    thread_exit(0);     /* a thread that simply returned did fine */
}

/* threads are all exactly the same size and get created and destroyed
 * constantly, which is precisely what an object cache is for */
static struct slab_cache thread_cache;

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg) {
    slab_cache_init(&thread_cache, "thread", sizeof(struct thread));

    struct thread *t = slab_alloc(&thread_cache);
    if (t == NULL) {
        return NULL;
    }

    /* one extra page at the bottom, which I then unmap. a thread that
     * runs off the end of its stack lands on that hole and takes a
     * clean page fault naming the address, instead of quietly chewing
     * through whatever the pmm handed out next -- which, on a kernel
     * with no memory protection between threads, would be some other
     * thread's stack and a bug you would chase for a week */
    uint64_t phys = pmm_alloc_pages(THREAD_STACK_PAGES + 1);
    if (phys == 0) {
        slab_free(t);
        return NULL;
    }

    t->stack_phys  = phys;
    t->stack_pages = THREAD_STACK_PAGES + 1;

    t->state       = THREAD_READY;
    t->wake_at     = 0;
    t->cpu_ticks   = 0;
    t->entry       = entry;
    t->arg         = arg;
    t->id          = next_id++;
    t->next        = NULL;
    t->wait_next   = NULL;
    t->from_heap   = true;
    t->space       = NULL;
    t->waiting_on  = NULL;
    t->pid         = 0;

    thread_set_name(t, name);

    /* punch the hole. the vmm may not exist yet if somebody creates a
     * thread before vmm_init, in which case the stack is merely
     * unguarded rather than broken */
    uint64_t guard = (uint64_t)pmm_phys_to_virt(phys);
    if (vmm_kernel_pml4() != 0) {
        vmm_unmap_page(vmm_kernel_pml4(), guard);
        vmm_flush_page(guard);
    }

    /* fabricate a stack that looks exactly like a thread which is
     * sitting inside switch_context waiting to be resumed. the pops
     * over there will eat my six zeroes, and its `ret` will land on
     * thread_bootstrap. stack top is page aligned, so the return
     * address slot ends up 16-aligned and bootstrap gets the stack
     * alignment the abi promises it */
    uint8_t *stack = pmm_phys_to_virt(phys + PAGE_SIZE);   /* past the guard */
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

void thread_exit(int code) {
    struct thread *me = sched_current();

    /* tell the process table before the thread goes, because the
     * process is what a parent will still be able to ask */
    bool narrate = true;
    if (me->pid != 0) {
        narrate = process_announces(me->pid);
        process_exited(me->pid, code, pit_uptime_ms());
    }

    /* a kernel thread always says so -- `summon` exists to be watched.
     * a program says so only when it was run to be watched */
    if (narrate) {
        kprintf("[%s] hath returned to the sea of souls\n", me->name);
    }

    uint64_t flags = irq_save();
    me->state = THREAD_DEAD;
    irq_restore(flags);

    /* the scheduler will never pick a dead thread, so this yield is a
     * one way door. the next thread to run reaps my stack out from
     * under me, which is only safe because I am never coming back */
    for (;;) {
        sched_yield();
    }
}

/* give a dead thread's stack back. the guard page has to be put back in
 * the direct map first: the pmm is about to hand that frame to somebody
 * else, and they will expect to be able to reach it */
void thread_free_stack(struct thread *t) {
    /* the address space owns the program's pages and its user stack, so
     * letting it go reclaims all of them at once */
    if (t->space != NULL) {
        addrspace_destroy(t->space);
        t->space = NULL;
    }
    if (t->stack_phys == 0) {
        return;     /* the boot thread's stack came from limine, not me */
    }

    uint64_t guard = (uint64_t)pmm_phys_to_virt(t->stack_phys);
    if (vmm_kernel_pml4() != 0) {
        vmm_map_range(vmm_kernel_pml4(), guard, t->stack_phys, PAGE_SIZE,
                      PTE_WRITE | vmm_nx());
        vmm_flush_page(guard);
    }

    pmm_free_pages(t->stack_phys, t->stack_pages);
}

/* the boot thread is a global that was never allocated, so it must not
 * be handed to any allocator. keeping that check here means the
 * scheduler does not have to know how a thread was made */
void thread_free(struct thread *t) {
    if (t == NULL || !t->from_heap) {
        return;
    }
    slab_free(t);
}
