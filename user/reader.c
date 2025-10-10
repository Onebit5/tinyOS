/* a program that opens a file.
 *
 * up to 0.1.1 a program could be *loaded* from the ramdisk but could
 * not read one -- `cat` had to be a kernel command, because only the
 * kernel could reach the archive. now there is `open`, and a file
 * descriptor is a thing a process owns. */

#include "syscall.h"

void _start(void) {
    write("[reader] i am pid ");
    write_num(getpid());
    write(", and i shall read something\n\n");

    long fd = open("motd.txt");
    if (fd < 0) {
        write("[reader] could not open motd.txt\n");
        exit(1);
    }

    /* in small bites, to show that the descriptor remembers where it
     * had got to between one read and the next */
    char buf[32];
    long total = 0;
    for (;;) {
        long n = read_fd(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        write_fd(STDOUT, buf, n);
        total += n;
    }

    close(fd);

    write("\n[reader] that was ");
    write_num(total);
    write(" bytes, in bites of 32\n");
    exit(0);
}
