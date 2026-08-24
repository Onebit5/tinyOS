#ifndef SCHED_USERMODE_H
#define SCHED_USERMODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* how big a stack ring 3 may grow to, and how much of it exists before
 * the program starts.
 *
 * up to 0.2.12 these were the same number and it was four: every
 * program got exactly four pages whether it used them or not, and
 * running off the end was a fault with nothing behind it.
 *
 * now the *range* is agreed to and the pages arrive as they are
 * touched. a program that uses a few hundred bytes of stack costs one
 * page; one that recurses gets more without anybody having decided in
 * advance how much it would need. and the address below the range is
 * still nothing at all, which is the guard page for free -- it costs no
 * memory because there is nothing there to cost anything */
#define USER_STACK_PAGES 256            /* a megabyte of room */
#define USER_STACK_EAGER 2              /* mapped before it starts */

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
/* the environment a program is born holding. NULL means an empty one */
struct spawn_env {
    const char *block;
    size_t      len;
};

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

/* what the next spawn should hand its child. set by the shell before
 * starting anything, because the shell is a kernel thread with no
 * process of its own to inherit from -- a program spawning a program
 * needs none of this and gets its parent's */
void user_spawn_env(const struct spawn_env *env);

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

/* a spawned process is created asleep, so that whoever started it can
 * settle its group, its descriptors and who holds the terminal before
 * anything runs. this is what lets it go */
void user_start(int pid);

/* a job: everything one typed line started, held together by a group
 * number so the terminal can talk to all of it at once */
struct job {
    int  pgid;
    int  pids[PIPELINE_MAX];
    int  count;
    bool stopped;       /* suspended by ctrl+z rather than finished */

    /* what the last of them exited with.
     *
     * the *last* rather than any of them, because that is what a
     * pipeline's status has meant since sh: `cat missing | wc -l`
     * succeeds, and it should -- wc did its job on an empty input.
     * nothing needed this until there was an `if` to read it */
    int  status;
};

/* wait for a job to end -- or to be stopped, which is the other way
 * waiting can finish. returns true if it really ended; false means it
 * is suspended and still there, which is what `fg` and `bg` are for.
 *
 * the terminal goes back to the shell either way */
bool user_job_wait(struct job *j);

/* let a stopped job go again, with or without the terminal. `fg` waits
 * for it afterwards; `bg` does not */
void user_job_continue(struct job *j, bool foreground);

/* is anything in it still going? */
bool user_job_alive(const struct job *j);

/* collect whatever has finished, so the table does not fill with the
 * remains of jobs nobody asked about */
void user_job_collect(struct job *j);

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
                   int uid, bool background, struct job *out,
                   const char **error);

/* block until a pid has ended, then collect it. false if there is no
 * such process. whether the caller had any business waiting for it is
 * the syscall layer's question, not this one's */
bool user_wait(int pid, int *code);

/* the shell's way in: spawn, and unless told otherwise wait for it and
 * report how it went */
bool user_run(const char *path, int argc, const char *const argv[],
              const char *cwd,
              int uid, bool background, bool announce, struct job *out,
              const char **error);

#endif
