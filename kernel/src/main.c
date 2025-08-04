#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "drivers/serial.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "lib/kprintf.h"
#include "lib/panic.h"

#define VERSION "0.0.5"

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
    gdt_init();
    idt_init();
    pic_init();
    keyboard_init();

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
    kprintf("tinyOS v%s\n\n", VERSION);

    /* velvet room blue, obviously */
    console_set_colors(0x7b8ce0, 0x101018);
    kprintf("Thou art I... And I am thou...\n");
    kprintf("Thou hast established a new bond...\n\n");
    kprintf("Thou shalt be blessed when creating\n");
    kprintf("Personas of the Computer's Arcana...\n\n");

    console_set_colors(0xc8c8d0, 0x101018);
    kprintf("framebuffer : %lux%lu @ %u bpp, pitch %lu bytes, at %p\n",
            fb->width, fb->height, fb->bpp, fb->pitch, fb->address);
    kprintf("font        : spleen 8x16 (bsd 2-clause)\n");
    kprintf("gdt         : loaded, tss slot reserved for later\n");
    kprintf("idt         : 256 gates armed, exceptions get caught now\n");
    kprintf("pic         : 8259 remapped to vectors 32-47, ghosts filtered\n");
    kprintf("keyboard    : ps/2 on irq1, us layout, listening\n");
    kprintf("kernel      : loaded at %p\n\n", (void *)kmain);

    asm volatile ("sti");

#ifdef FAULT_DEMO
    /* poke memory we very much dont own, to show off the exception
     * handler. build with `make clean && make FAULT_DEMO=1 iso` */
    kprintf("FAULT_DEMO: dereferencing 0xdeadbeef on purpose...\n");
    volatile uint64_t *bad = (volatile uint64_t *)0xdeadbeef;
    kprintf("read back %lx (if you see this something is very wrong)\n", *bad);
#endif

    console_set_colors(0x7b8ce0, 0x101018);
    kprintf("The contract hath been sealed.\n");
    kprintf("Speak thy will, and it shall be written...\n\n");
    console_set_colors(0xc8c8d0, 0x101018);
    kprintf("> ");

    /* the m3 demo: echo whatever gets typed. hlt naps between keys so we
     * arent spinning the cpu just to wait for a human. (yes, a key could
     * sneak in between the check and the hlt and sit in the buffer until
     * the next interrupt wakes us. the next keypress drains both. a real
     * blocking getchar comes with the scheduler) */
    for (;;) {
        int c = keyboard_getchar();
        if (c >= 0) {
            kprintf("%c", (char)c);
        } else {
            asm volatile ("hlt");
        }
    }
}
