#ifndef DRIVERS_PIT_H
#define DRIVERS_PIT_H

#include <stdint.h>

/* the 8253/8254 programmable interval timer. one of the oldest chips
 * still wired into a modern pc, and the easiest way to make time pass */

#define PIT_HZ 100      /* ticks per second, so one tick = 10ms */

void     pit_init(void);
uint64_t pit_ticks(void);
uint64_t pit_uptime_ms(void);

/* spin until n milliseconds have gone by, counting the ticks the timer
 * interrupt delivers. needs interrupts to be on and the timer to be
 * unmasked -- which is exactly what makes it useless for calibrating a
 * replacement for that timer. use pit_poll_wait for that */
void pit_busy_wait(uint64_t ms);

/* the same wait, with no interrupt involved at all: channel 2 counts
 * down and says so through a bit on the keyboard controller's port,
 * which I can simply read. this is how you measure one clock against
 * another before either of them is delivering anything.
 *
 * capped at 50ms, because channel 2 counts 16 bits at 1.193 MHz and
 * runs out a little past 54 */
void pit_poll_wait(uint64_t ms);

/* stop the pit interrupting. it keeps its tick counter -- uptime and
 * every sleep in the system are counted in those -- but something else
 * is doing the counting now */
void pit_stop(void);

/* the tick, from wherever it now comes. the lapic timer calls this once
 * the pit has stepped aside, so nothing above notices the change */
void pit_tick(void);

#endif
