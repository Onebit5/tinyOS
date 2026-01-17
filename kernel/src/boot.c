#include "boot.h"
#include "lib/panic.h"

static const struct ph_handoff *handoff;

void boot_take_handoff(const struct ph_handoff *h) {
    if (h == NULL || h->magic != PHILEMON_MAGIC) {
        /* nothing has been set up yet -- no console, no serial -- so
         * there is nowhere to complain to. stopping is the only honest
         * thing left */
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    handoff = h;
}

const struct ph_handoff *boot_handoff(void) {
    if (handoff == NULL) {
        panic("something asked about the boot before the boot happened");
    }
    return handoff;
}

uint64_t boot_hhdm(void) {
    return handoff != NULL ? handoff->hhdm : PHILEMON_HHDM;
}
