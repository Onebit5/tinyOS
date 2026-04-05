/* move, which is rename wearing a different hat.
 *
 * nothing is copied. a file's name lives in its directory and the file
 * itself lives in the clusters; changing the first does not touch the
 * second, which is why moving a hundred megabytes across directories
 * costs the same as moving nothing at all.
 *
 * the one thing that has to be got right is `mv a b/` where b is a
 * directory -- everybody expects that to mean b/a, and a rename that
 * took it literally would replace the directory with a file. */

#include "syscall.h"
#include "args.h"

static const struct opt mv_opts[] = {
    { 'v', "verbose", false, "say what went where" },
};

static const struct program mv = {
    .name = "mv",
    .usage = "mv [-v] <from> <to>",
    .summary = "move or rename a file",
    .opts = mv_opts,
    .opt_count = sizeof mv_opts / sizeof mv_opts[0],
};

static const char *basename(const char *path) {
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            last = p + 1;
        }
    }
    return last;
}

/* join, into a buffer whose size is known and respected. a truncated
 * path is a path to somewhere else */
static bool join(char *out, long cap, const char *dir, const char *name) {
    long n = 0;
    for (const char *p = dir; *p; p++) {
        if (n >= cap - 1) return false;
        out[n++] = *p;
    }
    if (n > 0 && out[n - 1] != '/') {
        if (n >= cap - 1) return false;
        out[n++] = '/';
    }
    for (const char *p = name; *p; p++) {
        if (n >= cap - 1) return false;
        out[n++] = *p;
    }
    out[n] = '\0';
    return true;
}

void _start(int argc, char **argv) {
    struct args a;
    const char *error;
    if (!args_parse(&mv, argc, argv, &a, &error)) {
        write("mv: ");
        write(error);
        write("\n");
        exit(1);
    }
    if (a.wants_help || a.count != 2) {
        args_usage(&mv);
        exit(a.wants_help ? 0 : 1);
    }

    const char *from = a.rest[0];
    const char *to = a.rest[1];

    struct stat st;
    if (stat(from, &st) < 0) {
        write("mv: no such file: ");
        write(from);
        write("\n");
        exit(1);
    }
    if (st.is_dir) {
        write("mv: ");
        write(from);
        write(" is a directory, and I cannot move one yet\n");
        exit(1);
    }

    /* `mv x somewhere` where somewhere is a directory means into it,
     * under the same name -- which is what everyone means by it */
    char joined[256];
    struct stat dest;
    if (stat(to, &dest) == 0) {
        if (dest.is_dir) {
            if (!join(joined, sizeof joined, to, basename(from))) {
                write("mv: that path is longer than I can hold\n");
                exit(1);
            }
            to = joined;
        } else {
            write("mv: ");
            write(to);
            write(" is already there\n");
            exit(1);
        }
    }

    if (rename(from, to) < 0) {
        write("mv: cannot move ");
        write(from);
        write(" to ");
        write(to);
        write("\n");
        exit(1);
    }

    if (args_has(&a, &mv, 'v')) {
        write(from);
        write(" -> ");
        write(to);
        write("\n");
    }
    exit(0);
}
