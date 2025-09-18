#ifndef SCHED_USERMODE_H
#define SCHED_USERMODE_H

#include <stdint.h>
#include <stdbool.h>

/* how big a stack ring 3 gets. it is not guarded -- the guard page
 * trick lives in the direct map, and this is a user mapping */
#define USER_STACK_PAGES 4

/* where user stacks go. low enough to be nowhere near the kernel, high
 * enough that a program's own segments will not collide with it */
#define USER_STACK_TOP 0x0000700000000000ull

/* implemented in usermode.asm. never returns */
void enter_usermode(uint64_t entry, uint64_t stack_top,
                    uint64_t cs, uint64_t ss);

/* the shell wants to say something more useful than this particular
 * message, so it is a value it can compare against rather than prose */
extern const char *const USER_RUN_NO_SUCH_FILE;

/* load an executable out of the ramdisk and run it as a ring 3 thread
 * in an address space of its own. foreground waits for it; background
 * returns as soon as it is running. returns false if the file is
 * missing or not something we can load */
bool user_run(const char *path, bool background, const char **error);

#endif
