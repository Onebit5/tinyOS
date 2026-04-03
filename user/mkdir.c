/* make a directory.
 *
 * the name has to fit 8.3, because that is all a short directory entry
 * can hold and minting long ones is a different job from reading them.
 * a name that will not fit is refused rather than quietly shortened --
 * a directory you cannot then name is worse than no directory. */

#include "syscall.h"
#include "args.h"

static const struct opt mkdir_opts[] = {
    { 'v', "verbose", false, "name each one as it is maked" },
};

static const struct program mkdir_prog = {
    .name = "mkdir",
    .usage = "mkdir [-v] <directory>...",
    .summary = "make a directory",
    .opts = mkdir_opts,
    .opt_count = sizeof mkdir_opts / sizeof mkdir_opts[0],
};

void _start(int argc, char **argv) {
    struct args a;
    const char *error;
    if (!args_parse(&mkdir_prog, argc, argv, &a, &error)) {
        write("mkdir: ");
        write(error);
        write("\n");
        exit(1);
    }
    if (a.wants_help || a.count == 0) {
        args_usage(&mkdir_prog);
        exit(a.wants_help ? 0 : 1);
    }

    long bad = 0;
    for (int i = 0; i < a.count; i++) {
        if (mkdir(a.rest[i]) < 0) {
            write("cannot make ");
            write(a.rest[i]);
            write("\n");
            bad++;
        } else if (args_has(&a, &mkdir_prog, 'v')) {
            write(a.rest[i]);
            write("\n");
        }
    }
    exit(bad == 0 ? 0 : 1);
}
