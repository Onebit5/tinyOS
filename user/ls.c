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
#include "args.h"

static const struct opt ls_opts[] = {
    { 'a', "all",  false, "count the directories too, not just the files" },
    { '1', "one",  false, "one name per line and nothing else" },
};

static const struct program ls = {
    .name = "ls",
    .usage = "ls [-a] [-1] [directory]",
    .summary = "what is in a directory",
    .opts = ls_opts,
    .opt_count = sizeof ls_opts / sizeof ls_opts[0],
};

static long ends_with_slash(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n > 0 && s[n - 1] == '/';
}

void _start(int argc, char **argv) {
    struct args a;
    const char *error;
    if (!args_parse(&ls, argc, argv, &a, &error)) {
        write("ls: ");
        write(error);
        write("\n");
        exit(1);
    }
    if (a.wants_help) {
        args_usage(&ls);
        exit(0);
    }

    const char *path = (a.count > 0) ? a.rest[0] : "/";
    bool plain = args_has(&a, &ls, '1');

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

        if (!plain) {
            write("  ");
        }
        write(name);
        write("\n");
    }

    if (files == 0 && dirs == 0) {
        write("nothing there, or no such directory\n");
        exit(1);
    }

    if (plain) {
        exit(0);
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
