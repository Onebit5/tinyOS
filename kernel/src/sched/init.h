#ifndef SCHED_INIT_H
#define SCHED_INIT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "sched/process.h"

/* init: the first process, and the one that owns the rest.
 *
 * until 0.2.19 `kmain` started the shells itself, because there was
 * nothing else to start them. that works exactly once. it gives the
 * machine no answer to any of the questions that come after boot -- what
 * order things come up in, what happens when one of them dies, who owns
 * a process whose parent has gone, and what "shut down" means beyond
 * pulling the plug in a hurry.
 *
 * so there is a process 1 now. it is the first thing in the table, it
 * outlives everything else, and it does four things:
 *
 *   it brings the machine up in an order,
 *   it restarts what dies -- and stops restarting what dies too often,
 *   it adopts processes whose parent has gone, and collects them,
 *   and it takes the machine down in the reverse of the order it came up.
 *
 * that last one is not ceremony. the disk has had a write-back cache
 * since 0.2.13, so a machine that resets while anything can still write
 * loses whatever had not reached the drive yet. everything has to be
 * *stopped* before the sync, and it can only be stopped in an order if
 * it was started in one. */

/* ---- what init supervises --------------------------------------------
 *
 * a service is a kernel thread init is prepared to start again. the four
 * console sessions are services and so is the disk flusher, which is the
 * whole of the list -- everything else on this machine is a program
 * somebody typed, and a program somebody typed should stay dead when it
 * ends.
 *
 * the state below is deliberately not "is the thread alive": that is a
 * question for the scheduler and init asks it every time round. what is
 * kept here is what the scheduler cannot answer -- how often this thing
 * has been dying, and whether init has given up on it. */

#define INIT_SERVICES_MAX  8
#define INIT_NAME_MAX      12

/* how many times a service may come back before init decides it is not
 * coming back, and over what stretch of time.
 *
 * something that dies instantly and is restarted instantly is a machine
 * that does nothing else ever again -- a login prompt that cannot draw
 * itself will happily consume every cycle there is, forever, printing
 * half a prompt. every init since sysvinit has had this rule and this is
 * why. the window matters as much as the count: five deaths in a second
 * is a loop, five deaths across an afternoon is five separate accidents
 * and each of them deserves a restart */
#define INIT_RESPAWN_MAX        5
#define INIT_RESPAWN_WINDOW_MS  10000

enum service_state {
    SERVICE_STOPPED = 0,    /* not running */
    SERVICE_RUNNING,
    SERVICE_GIVEN_UP,       /* died too often, too fast. left alone */
};

struct service {
    char  name[INIT_NAME_MAX];
    void (*entry)(void *);
    void *arg;

    /* which screen it belongs to. a thread inherits the console of
     * whoever made it, and everything here is made by init -- so a
     * session for console 3 has to be told it is for console 3 */
    unsigned console;

    /* should it be started again when it ends? true for a login
     * session, which is over when somebody logs out and wanted by the
     * next person who sits down */
    bool respawn;

    enum service_state state;
    int      thread_id;

    unsigned starts;            /* since boot, for `init` to print */
    unsigned in_window;         /* since the current window opened */
    uint64_t window_start_ms;
};

struct init_table {
    struct service s[INIT_SERVICES_MAX];
    size_t         count;
};

/* what to do about a service that has just ended */
enum init_action {
    INIT_LEAVE,         /* it was never meant to come back */
    INIT_RESTART,
    INIT_GIVE_UP,       /* it is dying in a loop. stop feeding it */
};

/* ---- the policy, which starts nothing --------------------------------
 *
 * split out from the thread that acts on it for the same reason the
 * mouse decoder is split from the mouse: what is worth testing here is
 * the *decisions*, and a decision that can only be reached by letting a
 * real service die five times on a real machine is a decision nobody
 * ever checks. these three take a table and a clock reading and touch
 * nothing else. */

void init_table_reset(struct init_table *t);

/* add one. false if the table is full */
bool init_add(struct init_table *t, const char *name, void (*entry)(void *),
              void *arg, unsigned console, bool respawn);

/* note that service `i` is now running as thread `tid` */
void init_started(struct init_table *t, size_t i, int tid, uint64_t now_ms);

/* and that it is not any more. returns what should happen next */
enum init_action init_died(struct init_table *t, size_t i, uint64_t now_ms);

/* bring one back that init had given up on, or that was never meant to
 * restart. clears the count, so it gets a full allowance again --
 * otherwise the first thing a revived service does is get given up on */
bool init_revive(struct init_table *t, size_t i);

/* find one by name. -1 if there is no such service */
int init_find(const struct init_table *t, const char *name);

const char *init_state_name(enum service_state s);

/* ---- the machinery ---------------------------------------------------
 *
 * declared here whether or not there is a machine to run it on, the way
 * the mouse driver declares mouse_init. the *definitions* are the half
 * that touches threads and reset lines and are guarded in init.c -- a
 * declaration costs nothing, and having it means a host test can stub
 * one of these deliberately rather than the header quietly hiding a name
 * that was supposed to exist */

/* make process 1 and the thread that runs it. everything else on this
 * machine is started by that thread rather than by kmain, which is the
 * entire point of the version */
bool init_boot(void);

/* how the machine stops */
enum init_stop {
    INIT_REBOOT,
    INIT_POWEROFF,
};

/* ask init to take the machine down. does not return: whoever typed
 * this is not going to need another prompt, and letting the shell carry
 * on printing one while the machine goes down underneath it looks
 * exactly like a machine that ignored the command */
void init_stop_machine(enum init_stop how) __attribute__((noreturn));

/* is the machine on its way down? the shells ask before printing a
 * prompt, so the last thing on a screen is the shutdown and not a
 * prompt nobody can type at */
bool init_stopping(void);

/* a copy of the table, for `init` to print. a copy because the real one
 * is init's and is written from init's thread */
void init_snapshot(struct init_table *out);

/* restart a service by name, for `init start <name>`. false if there is
 * no such service */
bool init_restart(const char *name);

#endif
