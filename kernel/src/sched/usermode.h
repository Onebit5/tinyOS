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

struct pipe;

/* where a program's standard input and output go, before it starts.
 *
 * everything here is optional and NULL means "the terminal", which is
 * what a program run on its own gets. a pipeline fills in the pipes; a
 * `>` or a `<` fills in a path. it is the same struct either way,
 * because from the program's side there is no difference at all */
struct spawn_io {
    struct pipe *in;            /* a pipe to read from */
    struct pipe *out;           /* a pipe to write to */
    const char  *in_path;       /* `< name`: read from this file */
    const char  *out_path;      /* `> name`: write to this file */
    bool         append;        /* `>>`: keep what is there and add to it */
};

/* start a program and return its pid, or 0 with *error set. `parent` is
 * the pid that will be allowed to wait for it -- 0 means the kernel
 * shell, which is nobody's child.
 *
 * the process owns whichever pipe ends it is given and lets go of them
 * when it dies, so the caller must not close them itself */
int user_spawn(const char *path, int argc, const char *const argv[],
               const char *cwd,
               int parent, int uid, bool announce,
               const struct spawn_io *io, const char **error);

/* one command in a pipeline: already resolved to a path, with the
 * arguments it was typed with and whatever redirection was written
 * beside it */
#define PIPELINE_MAX 4

struct stage {
    const char *path;
    int         argc;
    char      **argv;

    /* `<`, `>` and `>>` belong to a single command rather than to the
     * line, which is why they live here. `sort < a.txt > b.txt` is one
     * stage with both ends moved */
    const char *in_path;
    const char *out_path;
    bool        append;
};

/* run `count` commands with a pipe between each neighbouring pair, and
 * wait for all of them.
 *
 * they are all started before any is waited for, which is not an
 * optimisation but a requirement: a pipeline where the first is run to
 * completion before the second begins would deadlock the moment the
 * first wrote more than one buffer's worth. every stage runs at once
 * and the buffer between them is what keeps them in step.
 *
 * returns false with *error set if the first one could not be started
 * at all; a failure further along is reported and the rest carry on,
 * since they will see end of file and finish by themselves */
bool user_pipeline(const struct stage *stages, int count, const char *cwd,
                   int uid, bool background, const char **error);

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
