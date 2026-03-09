/* remove an empty directory.
 *
 * only an empty one. taking a whole tree away is a different operation
 * and ought to look like one at the point of asking, rather than being
 * something this quietly turns out to have done. */

#include "syscall.h"

void _start(int argc, char **argv) {
    if (argc < 2) {
        write("rmdir <directory>...\n");
        exit(1);
    }

    long bad = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            write("cannot remove ");
            write(argv[i]);
            write(" -- is it empty, and is it a directory?\n");
            bad++;
        }
    }
    exit(bad == 0 ? 0 : 1);
}
