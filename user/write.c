/* write a line to a file on the disk.
 *
 * there is no shell redirection here -- no pipes, no `>` -- so putting
 * something into a file needs a program that takes both the name and
 * the words. it is the plainest possible demonstration of the thing
 * that actually matters about this milestone: what you type survives
 * the machine being turned off.
 *
 * appends rather than truncating, so running it twice builds something
 * up rather than replacing it. */

#include "syscall.h"
#include "args.h"

static const struct opt write_opts[] = {
    { 't', "truncate", false, "replace what is there instead of adding to it" },
};

static const struct program write_prog = {
    .name = "write",
    .usage = "write [-t] <file> <words...>",
    .summary = "put words in a file on the disk",
    .opts = write_opts,
    .opt_count = sizeof write_opts / sizeof write_opts[0],
};

void _start(int argc, char **argv) {
    struct args a;
    const char *error;
    if (!args_parse(&write_prog, argc, argv, &a, &error)) {
        write("write: ");
        write(error);
        write("\n");
        exit(1);
    }
    if (a.wants_help || a.count < 2) {
        write(write_prog.usage);
        write("\n");
        write("try: write /notes.txt the bond endures\n");
        exit(a.wants_help ? 0 : 1);
    }

    long fd = create(a.rest[0]);
    if (fd < 0) {
        write("cannot write to ");
        write(a.rest[0]);
        write("\n");
        write("only the disk can be written to, the name must fit 8.3, ");
        write("and only the master may do it\n");
        exit(1);
    }

    /* create leaves me at the start. going to the end means adding to
     * what is there, which is what running this twice ought to do --
     * unless it was asked to replace instead */
    if (!args_has(&a, &write_prog, 't')) {
        char scratch[256];
        for (;;) {
            long n = read_fd(fd, scratch, sizeof scratch);
            if (n <= 0) {
                break;
            }
        }
    }

    long written = 0;
    for (int i = 1; i < a.count; i++) {
        long n = write_fd(fd, a.rest[i], (long)ustrlen(a.rest[i]));
        if (n < 0) {
            write("the write was refused partway through\n");
            exit(1);
        }
        written += n;

        const char *tail = (i + 1 < a.count) ? " " : "\n";
        n = write_fd(fd, tail, 1);
        if (n > 0) {
            written += n;
        }
    }

    close(fd);

    write_num(written);
    write(" bytes are now on the disk, and will still be there ");
    write("after the power goes\n");
    exit(0);
}
