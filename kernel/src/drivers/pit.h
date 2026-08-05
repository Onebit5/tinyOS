#ifndef DRIVERS_PIT_H
#define DRIVERS_PIT_H

#include <stdint.h>

/* the 8253/8254 programmable interval timer. one of the oldest chips
 * still wired into a modern pc, and the easiest way to make time pass */

#define PIT_HZ 100      /* ticks per second, so one tick = 10ms */

void     pit_init(void);
uint64_t pit_ticks(void);
uint64_t pit_uptime_ms(void);

/* spin until n milliseconds have gone by. this is the dumb version that
 * burns the cpu -- it exists for the stretch of boot before the
 * scheduler is alive. once threads exist, use sleep_ms() instead */
void pit_busy_wait(uint64_t ms);

#endif
