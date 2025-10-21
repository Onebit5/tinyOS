/* ls, which used to be a kernel command.
 *
 * this one genuinely needed something new: a way to ask what is in the
 * ramdisk. `open` can only answer about a name you already know. so
 * SYS_READDIR exists now -- an index and a name, which is the whole of
 * a directory when there are no directories. */

#include "syscall.h"

static long ends_with_slash(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n > 0 && s[n - 1] == '/';
}

void _start(int argc, char **argv) {
    (void)argc; (void)argv;

    char name[128];
    long files = 0;

    for (long i = 0; ; i++) {
        if (readdir(i, name, sizeof name) < 0) {
            break;
        }
        /* tar keeps directory entries; they have nothing to open */
        if (name[0] == '\0' || ends_with_slash(name)) {
            continue;
        }
        write("  ");
        write(name);
        write("\n");
        files++;
    }

    write_num(files);
    write(" files\n");
    exit(0);
}
