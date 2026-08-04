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
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"

#define VERSION "0.0.6"

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

/* the m4 demo: put the fresh allocators through their paces at boot.
 * every failure panics, so reaching the prompt means it all held */
static void memory_selftest(void) {
    uint64_t free_before = pmm_free_bytes();

    /* pmm round trip: 8 frames, distinct patterns, verify, return */
    uint64_t frames[8];
    for (int i = 0; i < 8; i++) {
        frames[i] = pmm_alloc();
        if (frames[i] == 0) {
            panic("selftest: the pmm ran dry after %d pages", i);
        }
        memset(pmm_phys_to_virt(frames[i]), 0xa5 + i, PAGE_SIZE);
    }
    for (int i = 0; i < 8; i++) {
        uint8_t *p = pmm_phys_to_virt(frames[i]);
        for (int j = 0; j < PAGE_SIZE; j++) {
            if (p[j] != (uint8_t)(0xa5 + i)) {
                panic("selftest: frame %d forgot its pattern at byte %d", i, j);
            }
        }
        pmm_free(frames[i]);
    }
    if (pmm_free_bytes() != free_before) {
        panic("selftest: pmm books dont balance after round trip");
    }

    /* heap round trip: mixed sizes incl one bigger than a whole page,
     * scribble, verify, free in shuffled order, books must balance */
    uint64_t used_before = kheap_used_bytes();
    size_t sizes[5] = { 24, 1000, 16384, 1, 512 };
    uint8_t *ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (ptrs[i] == NULL) {
            panic("selftest: kmalloc(%zu) said no", sizes[i]);
        }
        memset(ptrs[i], 0x30 + i, sizes[i]);
    }
    for (int i = 0; i < 5; i++) {
        for (size_t j = 0; j < sizes[i]; j++) {
            if (ptrs[i][j] != (uint8_t)(0x30 + i)) {
                panic("selftest: heap block %d got trampled at byte %zu", i, j);
            }
        }
    }
    int order[5] = { 2, 0, 4, 1, 3 };
    for (int i = 0; i < 5; i++) {
        kfree(ptrs[order[i]]);
    }
    if (kheap_used_bytes() != used_before) {
        panic("selftest: heap books dont balance after round trip");
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

    pmm_init();
    memory_selftest();
    kprintf("  -> selftest: 8 frames + 5 heap blocks round-tripped, books balance\n");
    kprintf("memory      : %lu MiB free of %lu MiB, heap warmed to %lu KiB\n\n",
            pmm_free_bytes() / (1024 * 1024),
            pmm_total_bytes() / (1024 * 1024),
            kheap_total_bytes() / 1024);

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
