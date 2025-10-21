/* a program that reads the keyboard.
 *
 * this could not work before 0.1.3. the shell used to sit in a loop
 * peeking at keys while a program ran, so anything a program tried to
 * read had already been taken. now the terminal has a foreground
 * process, and while this runs, that is us.
 *
 * ctrl+c is delivered here rather than acted on for us: a read comes
 * back -1 and we get to decide what that means. pressing it twice says
 * the kernel should stop asking nicely. */

#include "syscall.h"

static void prompt_for(const char *what, char *buf, long max) {
    write(what);
    long n = read_fd(STDIN, buf, max - 1);
    if (n < 0) {
        buf[0] = '\0';
        return;
    }
    /* the newline came with it */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        n--;
    }
    buf[n] = '\0';
}

void _start(int argc, char **argv) {
    (void)argc; (void)argv;

    char name[64];

    write("[ask] i am pid ");
    write_num(getpid());
    write(", and the keyboard is mine while i run\n\n");

    prompt_for("  what is thy name? ", name, sizeof name);

    if (name[0] == '\0') {
        write("\n[ask] interrupted. i shall ask no more\n");
        exit(130);          /* what a shell would call SIGINT */
    }

    write("\n  well met, ");
    write(name);
    write(".\n\n");

    write("[ask] now say nothing, and press ctrl+c to interrupt me\n");
    for (int i = 0; i < 20; i++) {
        if (sleep(500) < 0) {
            write("[ask] interrupted mid-sleep. leaving politely\n");
            exit(130);
        }
        write("  still waiting...\n");
    }

    write("[ask] nobody interrupted me. how patient thou art\n");
    exit(0);
}
