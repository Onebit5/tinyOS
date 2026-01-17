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

void _start(int argc, char **argv) {
    if (argc < 3) {
        write("write <file on the disk> <words...>\n");
        write("try: write /disk/notes.txt the bond endures\n");
        exit(1);
    }

    long fd = create(argv[1]);
    if (fd < 0) {
        write("cannot write to ");
        write(argv[1]);
        write("\n");
        write("only /disk can be written to, the name must fit 8.3, ");
        write("and only the master may do it\n");
        exit(1);
    }

    /* create leaves me at the start; go to the end so a second run adds
     * to the file rather than writing over what is already there */
    char scratch[256];
    long at = 0;
    for (;;) {
        long n = read_fd(fd, scratch, sizeof scratch);
        if (n <= 0) {
            break;
        }
        at += n;
    }

    long written = 0;
    for (int i = 2; i < argc; i++) {
        long n = write_fd(fd, argv[i], (long)ustrlen(argv[i]));
        if (n < 0) {
            write("the write was refused partway through\n");
            exit(1);
        }
        written += n;

        const char *tail = (i + 1 < argc) ? " " : "\n";
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
