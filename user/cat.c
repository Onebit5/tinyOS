/* cat, which used to be a kernel command.
 *
 * it needed `open` and `read`, which arrived in 0.1.2, and arguments,
 * which arrived here. nothing else -- no kernel internals at all, which
 * is why it was always the obvious first thing to move out. */

#include "syscall.h"

static int show(const char *path) {
    long fd = open(path);
    if (fd < 0) {
        write("cat: no such file: ");
        write(path);
        write("\n");
        return 1;
    }

    char buf[128];
    long last = 0;
    for (;;) {
        long n = read_fd(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        write_fd(STDOUT, buf, n);
        last = buf[n - 1];
    }
    close(fd);

    if (last != '\n') {
        write("\n");
    }
    return 0;
}

void _start(int argc, char **argv) {
    if (argc < 2) {
        write("cat <file> [file...]\n");
        exit(1);
    }

    int bad = 0;
    for (int i = 1; i < argc; i++) {
        bad |= show(argv[i]);
    }
    exit(bad);
}
