/* make a directory.
 *
 * the name has to fit 8.3, because that is all a short directory entry
 * can hold and minting long ones is a different job from reading them.
 * a name that will not fit is refused rather than quietly shortened --
 * a directory you cannot then name is worse than no directory. */

#include "syscall.h"

void _start(int argc, char **argv) {
    if (argc < 2) {
        write("mkdir <directory>...\n");
        exit(1);
    }

    long bad = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i]) < 0) {
            write("cannot make ");
            write(argv[i]);
            write("\n");
            bad++;
        }
    }
    exit(bad == 0 ? 0 : 1);
}
