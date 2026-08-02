#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "drivers/serial.h"

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

/* quick and dirty decimal printer over serial.
 * TODO: rip this out the moment kprintf exists */
static void serial_write_u64(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v > 0);
    serial_write(&buf[i]);
}

void kmain(void) {
    /* if the bootloader doesnt speak our base revision theres not much
     * we can do about it, so just park the cpu */
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    serial_init();
    serial_write("\ntinyOS 0.0.2 -- hello from long mode\n");

    if (framebuffer_request.response == NULL
        || framebuffer_request.response->framebuffer_count < 1) {
        serial_write("limine gave us no framebuffer :( parking\n");
        hcf();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    serial_write("framebuffer: ");
    serial_write_u64(fb->width);
    serial_write("x");
    serial_write_u64(fb->height);
    serial_write(", ");
    serial_write_u64(fb->bpp);
    serial_write(" bpp\n");

    /* qemu/limine hand us 32bpp basically always, so thats all we handle.
     * pitch is in bytes, we index in pixels */
    volatile uint32_t *px = fb->address;
    size_t stride = fb->pitch / 4;

    /* dark background, so its obvious we own every pixel now */
    for (size_t y = 0; y < fb->height; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            px[y * stride + x] = 0x101018;
        }
    }

    /* the classic "yes the framebuffer really works" diagonal stripes */
    for (size_t i = 0; i < 256 && i < fb->height && i + 64 < fb->width; i++) {
        px[i * stride + i]      = 0xe64553;   /* red-ish */
        px[i * stride + i + 32] = 0x45e653;   /* green-ish */
        px[i * stride + i + 64] = 0x5345e6;   /* blue-ish */
    }

    serial_write("test pattern drawn. nothing left to do, parking. bye\n");
    hcf();
}
