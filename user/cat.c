/* cat, which used to be a kernel command.
 *
 * it needed `open` and `read`, which arrived in 0.1.2, and arguments,
 * which arrived here. nothing else -- no kernel internals at all, which
 * is why it was always the obvious first thing to move out. */

#include "syscall.h"
#include "args.h"

/* what this program takes, declared once. the parser reads it, and so
 * does anything that has to explain the program to somebody -- which is
 * what keeps a usage line from drifting away from what the code does */
static const struct opt cat_opts[] = {
    { 'v', "verbose", false, "name each file and its size before its contents" },
    { 'n', "number",  false, "number the lines" },
};

static const struct program cat = {
    .name = "cat",
    .usage = "cat [-v] [-n] <file>...",
    .summary = "print files",
    .opts = cat_opts,
    .opt_count = sizeof cat_opts / sizeof cat_opts[0],
};

static bool verbose;
static bool numbered;
static long line_no = 1;

static int show(const char *path) {
    long fd = open(path);
    if (fd < 0) {
        write("cat: no such file: ");
        write(path);
        write("\n");
        return 1;
    }

    if (verbose) {
        write("==> ");
        write(path);
        write(" <==\n");
    }

    char buf[128];
    long last = '\n';
    for (;;) {
        long n = read_fd(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        if (!numbered) {
            write_fd(STDOUT, buf, n);
            last = buf[n - 1];
            continue;
        }
        /* one byte at a time, because a line can end anywhere in a
         * buffer and the number goes at the start of the next one */
        for (long i = 0; i < n; i++) {
            if (last == '\n') {
                write_num(line_no++);
                write("  ");
            }
            write_fd(STDOUT, &buf[i], 1);
            last = buf[i];
        }
    }
    close(fd);

    if (last != '\n') {
        write("\n");
    }
    return 0;
}

void _start(int argc, char **argv) {
    struct args a;
    const char *error;
    if (!args_parse(&cat, argc, argv, &a, &error)) {
        write("cat: ");
        write(error);
        write("\n");
        exit(1);
    }
    if (a.wants_help || a.count == 0) {
        write(cat.usage);
        write("\n");
        exit(a.wants_help ? 0 : 1);
    }

    verbose = args_has(&a, &cat, 'v');
    numbered = args_has(&a, &cat, 'n');

    int bad = 0;
    for (int i = 0; i < a.count; i++) {
        bad += show(a.rest[i]);
    }
    exit(bad == 0 ? 0 : 1);
}
