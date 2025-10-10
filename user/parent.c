/* a program that starts another program.
 *
 * this is the pair the roadmap called the point of 0.1.2: with `spawn`
 * and `wait`, something in ring 3 can do what only the kernel shell
 * could do before. a shell that is itself a program is now a thing
 * that could be written, which is what 0.1.4 is for. */

#include "syscall.h"

void _start(void) {
    write("[parent] i am pid ");
    write_num(getpid());
    write("\n");

    write("[parent] starting bin/fail, which i expect to go badly\n");
    long child = spawn("bin/fail");
    if (child < 0) {
        write("[parent] could not start it\n");
        exit(1);
    }

    write("[parent] it is pid ");
    write_num(child);
    write(", and i shall wait\n");

    int code = 0;
    if (wait(child, &code) < 0) {
        write("[parent] waiting failed\n");
        exit(1);
    }

    write("[parent] it exited ");
    write_num(code);
    write(", exactly as foretold\n");

    /* that number crossed two address spaces and outlived the thread
     * that produced it. pass it on as our own */
    exit(code);
}
