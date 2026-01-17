/* ls.
 *
 * there is one namespace now: the disk at `/`, and the ramdisk at
 * `/boot` where the programs and passwd live. so this no longer has to
 * know which filesystem it is looking at -- it names a directory and
 * the kernel works out who owns it.
 *
 * directories come back with a slash on the end, which is how everyone
 * has said "this one can be descended into" since long before any of
 * this. */

#include "syscall.h"

static long ends_with_slash(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n > 0 && s[n - 1] == '/';
}

void _start(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";

    char name[128];
    long files = 0;
    long dirs = 0;

    for (long i = 0; ; i++) {
        if (readdir_at(i, name, sizeof name, path) < 0) {
            break;
        }
        if (name[0] == '\0') {
            continue;
        }

        if (ends_with_slash(name)) {
            dirs++;
        } else {
            files++;
        }

        write("  ");
        write(name);
        write("\n");
    }

    if (files == 0 && dirs == 0) {
        write("nothing there, or no such directory\n");
        exit(1);
    }

    write_num(files);
    write(" files");
    if (dirs > 0) {
        write(", ");
        write_num(dirs);
        write(" directories");
    }
    write("\n");
    exit(0);
}
