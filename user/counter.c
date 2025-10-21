/* a second program, so there can be two.
 *
 * it links to exactly the same addresses as hello -- same entry point,
 * same stack -- and two copies can run at once without meeting, which
 * is only true because each one gets its own page tables. run it twice
 * with `run bin/counter &` and watch them not interfere. */

#include "syscall.h"

void _start(int argc, char **argv) {
    (void)argc; (void)argv;

    /* a page of our own, at an address the other copy also thinks it
     * owns. the number in here proves nobody else is writing to it */
    static long private_count;

    write("[counter] awake, and this memory is mine alone\n");

    for (int i = 0; i < 12; i++) {
        private_count++;
        write("[counter] mine says ");
        write_num(private_count);
        write(", uptime ");
        write_num(uptime());
        write("ms\n");
        sleep(600);
    }

    write("[counter] done, and it never once saw the other\n");
    exit(0);
}
