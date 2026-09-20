#ifndef ARCH_INLINE_H
#define ARCH_INLINE_H

/* an arch primitive has to *be* its instruction, where it is written.
 *
 * this kernel builds at -O0, where a plain `static inline` is a call
 * like any other. for most of these that is merely slower than the
 * inline assembly they replaced -- a call to a function containing
 * `pause` still pauses.
 *
 * for the two that read a register it is not slower, it is wrong. a
 * called function reads *its own* frame and *its own* stack, so
 * cpu_frame_pointer() would hand back its own frame and the backtrace
 * would start one line too low, politely reporting itself as the
 * innermost caller. that is the kind of wrong that looks plausible.
 *
 * so they say so, rather than hoping for an optimiser that this build
 * does not turn on. */
#define ARCH_INLINE static inline __attribute__((always_inline))

#endif
