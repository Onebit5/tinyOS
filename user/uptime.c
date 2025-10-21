/* uptime, which used to be a kernel command.
 *
 * it needed one syscall that has existed since ring 3 did. the only
 * reason it lived in the kernel was that there was nowhere else for a
 * command to live. */

#include "syscall.h"

void _start(int argc, char **argv) {
    (void)argc; (void)argv;

    long ms = uptime();
    long s = ms / 1000;

    write("awake for ");
    write_num(s / 3600);
    write("h ");
    write_num((s / 60) % 60);
    write("m ");
    write_num(s % 60);
    write("s (");
    write_num(ms);
    write("ms)\n");
    exit(0);
}
