#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "drivers/serial.h"
#include "drivers/console.h"
#include "lib/kprintf.h"
#include "lib/panic.h"

#define VERSION "0.0.3"

/* limine protocol stuff. these markers have to live in their own section
 * (see linker.ld) or the bootloader never finds us and we boot into a
 * black screen of nothing. ask me how i know. */

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

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
    /* too early to even panic() properly, so just park */
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    serial_init();

    if (framebuffer_request.response == NULL
        || framebuffer_request.response->framebuffer_count < 1) {
        panic("limine handed us no framebuffer, cant even draw a sad face");
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    console_init(fb);
    if (!console_ready()) {
        kprintf("console refused %u bpp, serial only from here\n", fb->bpp);
    }

    /* the banner. green name because we earned it */
    console_set_colors(0x45e653, 0x101018);
    kprintf("tinyOS v%s\n", VERSION);
    console_set_colors(0xc8c8d0, 0x101018);
    kprintf("hello from long mode, higher half edition\n\n");

    kprintf("framebuffer : %lux%lu @ %u bpp, pitch %lu bytes, at %p\n",
            fb->width, fb->height, fb->bpp, fb->pitch, fb->address);
    kprintf("font        : spleen 8x16 (bsd 2-clause)\n");
    kprintf("kernel      : loaded at %p\n\n", (void *)kmain);

    kprintf("nothing else to do yet. parking the cpu, bye\n");
    hcf();
}
