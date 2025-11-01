/* whoami, from the other side of the boundary.
 *
 * the kernel has a builtin of this name too, and the difference is the
 * point: that one reads a variable the shell keeps, this one asks the
 * kernel what uid the process it is running in was given. a program
 * cannot lie about it, because it was never told in a way it could
 * change -- the number lives on the process, in the kernel's memory,
 * which is exactly the memory ring 3 cannot reach. */

#include "syscall.h"

void _start(int argc, char **argv) {
    (void)argc; (void)argv;

    long uid = getuid();

    write("this program is pid ");
    write_num(getpid());
    write(", running as uid ");
    write_num(uid);
    write("\n");

    /* and the boundary, demonstrated rather than described */
    write("\ntrying to read velvet-room.txt:\n");
    long fd = open("velvet-room.txt");
    if (fd < 0) {
        write("  refused. the kernel checked the mode against my uid,\n");
        write("  and there is nothing i can do about its answer.\n");
        exit(1);
    }

    char buf[128];
    for (;;) {
        long n = read_fd(fd, buf, sizeof buf);
        if (n <= 0) break;
        write_fd(STDOUT, buf, n);
    }
    close(fd);
    exit(0);
}
