/* the first program tinyOS ever ran that it did not also contain.
 *
 * this is ring 3: it cannot touch a port, cannot read the kernel, and
 * cannot see any memory but its own. the only thing it can do to the
 * outside world is ask, through `syscall`, and be answered. */

#include "syscall.h"

void _start(void) {
    write("\n");
    write("I am thou... thou art I...\n");
    write("A voice speaks from ring 3, where it can touch nothing\n");
    write("and must ask for everything.\n\n");

    write("  privilege   3 (the outer ring)\n");
    write("  uptime      ");
    write_num(uptime());
    write(" ms since the bond was formed\n\n");

    write("Counting, so thou may watch the wheel keep turning:\n");
    for (int i = 1; i <= 5; i++) {
        write("  ");
        write_num(i);
        write(" ... the kernel yet lives\n");
        sleep(400);
    }

    write("\nMy purpose is fulfilled. Returning to the sea of souls.\n\n");
    exit(0);
}
