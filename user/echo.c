/* echo, which used to be a kernel command.
 *
 * it needed nothing from the kernel except a way to be handed its
 * arguments -- which is the whole reason it could not move out until
 * now. argc and argv arrive in rdi and rsi, laid out on this program's
 * own stack before it ever ran. */

#include "syscall.h"

void _start(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        write(argv[i]);
        if (i + 1 < argc) {
            write(" ");
        }
    }
    write("\n");
    exit(0);
}
