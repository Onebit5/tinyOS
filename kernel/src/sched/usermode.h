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

/* how many arguments a program may be handed. the shell's own argv is
 * the same width, so nothing is lost passing through it */
#define MAX_ARGS 8

/* implemented in usermode.asm. never returns */
void enter_usermode(uint64_t entry, uint64_t stack_top,
                    uint64_t cs, uint64_t ss,
                    uint64_t argc, uint64_t argv);

/* the shell wants to say something more useful than this particular
 * message, so it is a value it can compare against rather than prose */
extern const char *const USER_RUN_NO_SUCH_FILE;

/* start a program and return its pid, or 0 with *error set. `parent` is
 * the pid that will be allowed to wait for it -- 0 means the kernel
 * shell, which is nobody's child */
int user_spawn(const char *path, int argc, const char *const argv[],
               const char *cwd,
               int parent, int uid, bool announce, const char **error);

/* block until a pid has ended, then collect it. false if there is no
 * such process. whether the caller had any business waiting for it is
 * the syscall layer's question, not this one's */
bool user_wait(int pid, int *code);

/* the shell's way in: spawn, and unless told otherwise wait for it and
 * report how it went */
bool user_run(const char *path, int argc, const char *const argv[],
              const char *cwd,
              int uid, bool background, bool announce, const char **error);

#endif
