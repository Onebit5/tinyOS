#include "drivers/pit.h"
#include "cpu/io.h"
#include "cpu/pic.h"
#include "cpu/interrupts.h"
#include "sched/sched.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_IRQ  0

/* the crystal runs at 1.193182 MHz because of a 1981 decision to reuse
 * the ntsc colorburst divider. we have all been living with it since */
#define PIT_BASE_HZ 1193182

static volatile uint64_t ticks;

static void pit_irq(struct interrupt_frame *f) {
    (void)f;
    ticks++;
    /* hand the tick to the scheduler, which decides if the running
     * thread has had enough of the cpu */
    sched_tick();
}

void pit_init(void) {
    uint16_t divisor = PIT_BASE_HZ / PIT_HZ;

    /* channel 0, lobyte+hibyte access, mode 2 (rate generator), binary */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0, divisor & 0xff);
    outb(PIT_CH0, divisor >> 8);

    irq_register(PIT_IRQ, pit_irq);
    pic_unmask(PIT_IRQ);
}

uint64_t pit_ticks(void) {
    return ticks;
}

uint64_t pit_uptime_ms(void) {
    return ticks * (1000 / PIT_HZ);
}

void pit_busy_wait(uint64_t ms) {
    uint64_t until = ticks + (ms / (1000 / PIT_HZ)) + 1;
    while (ticks < until) {
        asm volatile ("pause");
    }
}
