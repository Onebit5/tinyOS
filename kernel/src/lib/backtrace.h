#ifndef LIB_BACKTRACE_H
#define LIB_BACKTRACE_H

#include <stdint.h>

/* walk the saved frame pointers and print who called whom.
 *
 * pass 0 for rbp to start from wherever the caller is; pass a saved
 * rbp (out of an interrupt frame, say) to walk somebody else's stack.
 * rip, if non-zero, gets printed as the innermost frame -- useful from
 * an exception handler, where the faulting instruction is not on the
 * frame chain at all */
/* named kbacktrace, not backtrace, because glibc has a backtrace() and
 * the host tests link against it -- ours was quietly being shadowed */
void kbacktrace(uint64_t rbp, uint64_t rip);

#endif
