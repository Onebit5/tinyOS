#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"

/* limine protocol stuff. these markers have to live in their own section
 * (see linker.ld) or the bootloader never finds us and we boot into a
 * black screen of nothing. ask me how i know. */

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* halt and catch fire */
static void hcf(void) {
    for (;;) {
        asm volatile ("hlt");
    }
}

void kmain(void) {
    /* if the bootloader doesnt speak our base revision theres not much
     * we can do about it, so just park the cpu */
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    /* m0: we made it to long mode, higher half, executing our own code.
     * thats the whole milestone. framebuffer + serial come in m1 */
    hcf();
}
