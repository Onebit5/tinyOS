/* chown: give a file to somebody else.
 *
 * only the master may, and that is not an arbitrary rule. if anybody
 * could hand a file away then an owner is a suggestion -- and every
 * question of the form "may you read this" turns into "did you
 * remember to check who is asking", which is the same question with
 * more steps and one more place to get it wrong. */

#include "syscall.h"
#include "args.h"

static const struct opt chown_opts[] = {
    { 'v', "verbose", false, "name each one as it changes" },
};

static const struct program chown_prog = {
    .name = "chown",
    .usage = "chown <uid> <file>...",
    .summary = "give a file to another user (the master only)",
    .opts = chown_opts,
    .opt_count = sizeof chown_opts / sizeof chown_opts[0],
};

void _start(int argc, char **argv) {
    struct args a;
    const char *error;
    if (!args_parse(&chown_prog, argc, argv, &a, &error)) {
        write("chown: ");
        write(error);
        write("\n");
        exit(1);
    }
    if (a.wants_help || a.count < 2) {
        args_usage(&chown_prog);
        exit(a.wants_help ? 0 : 1);
    }

    long uid = 0;
    for (const char *p = a.rest[0]; *p; p++) {
        if (*p < '0' || *p > '9') {
            write("chown: a user is a number here -- there is no name "
                  "service to ask\n");
            exit(1);
        }
        uid = uid * 10 + (*p - '0');
    }

    long bad = 0;
    for (int i = 1; i < a.count; i++) {
        if (chown(a.rest[i], uid, uid) < 0) {
            write("chown: cannot change ");
            write(a.rest[i]);
            write("\n");
            bad++;
        } else if (args_has(&a, &chown_prog, 'v')) {
            write(a.rest[i]);
            write("\n");
        }
    }
    exit(bad == 0 ? 0 : 1);
}
