/* ls, which used to be a kernel command.
 *
 * this one genuinely needed something new: a way to ask what is in a
 * filesystem. `open` can only answer about a name you already know. so
 * SYS_READDIR exists -- an index and a name.
 *
 * with a disk mounted there are two filesystems now, and which one you
 * mean is decided by whether you name a path. no argument is the
 * ramdisk, which has no directories to name; `ls /disk` is the disk,
 * where directories come back with a slash on the end. */

#include "syscall.h"

static long ends_with_slash(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n > 0 && s[n - 1] == '/';
}

void _start(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : 0;

    char name[128];
    long files = 0;
    long dirs = 0;

    for (long i = 0; ; i++) {
        long n = path ? readdir_at(i, name, sizeof name, path)
                      : readdir(i, name, sizeof name);
        if (n < 0) {
            break;
        }
        if (name[0] == '\0') {
            continue;
        }

        if (ends_with_slash(name)) {
            /* on the disk that means a directory, worth showing. in the
             * ramdisk it is one of tar's directory records, which has
             * nothing behind it to open */
            if (!path) {
                continue;
            }
            dirs++;
        } else {
            files++;
        }

        write("  ");
        write(name);
        write("\n");
    }

    if (files == 0 && dirs == 0 && path) {
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
