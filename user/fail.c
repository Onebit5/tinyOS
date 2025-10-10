/* a program that goes wrong on purpose.
 *
 * it exists so the exit code has somewhere to come from. the kernel
 * carries the number back to whoever waited, which is only possible
 * because the process outlives the thread that ran it -- the thread and
 * its whole address space are gone by the time anyone reads this. */

#include "syscall.h"

void _start(void) {
    write("[fail] i shall attempt something beyond me\n");
    sleep(300);
    write("[fail] as expected, it did not go well\n");
    exit(42);
}
