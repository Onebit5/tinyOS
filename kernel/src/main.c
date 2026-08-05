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
#include "drivers/pit.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "sched/sched.h"
#include "sched/thread.h"

#define VERSION "0.0.7"

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

/* two personas of the Computer Arcana, summoned to prove that the cpu
 * really is being taken away from them and handed back. they sleep
 * between lines, so most of the time neither of them is runnable and
 * the idle thread gets the cpu instead */

static void pixie_thread(void *arg) {
    uint64_t period = (uint64_t)(uintptr_t)arg;
    for (uint64_t n = 1; ; n++) {
        kprintf("[pixie]      count %lu, uptime %lums\n", n, pit_uptime_ms());
        sleep_ms(period);
    }
}

static void jack_frost_thread(void *arg) {
    uint64_t period = (uint64_t)(uintptr_t)arg;
    for (uint64_t n = 1; ; n++) {
        kprintf("[jack-frost] hee-ho! %lu, uptime %lums\n", n, pit_uptime_ms());
        sleep_ms(period);
    }
}

/* a thread that does its business and leaves, so the reaper has
 * something to clean up and you can watch it happen */
static void herald_thread(void *arg) {
    (void)arg;
    sleep_ms(2000);
    kprintf("[herald] my purpose is fulfilled. fare thee well\n");
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
    kprintf("timer       : pit channel 0 at %u hz, %ums per tick\n",
            PIT_HZ, 1000 / PIT_HZ);
    kprintf("kernel      : loaded at %p\n\n", (void *)kmain);

    pmm_init();
    memory_selftest();
    kprintf("  -> selftest: 8 frames + 5 heap blocks round-tripped, books balance\n");
    kprintf("memory      : %lu MiB free of %lu MiB, heap warmed to %lu KiB\n\n",
            pmm_free_bytes() / (1024 * 1024),
            pmm_total_bytes() / (1024 * 1024),
            kheap_total_bytes() / 1024);

    /* from here on this function is a thread like any other */
    sched_init();
    pit_init();

    thread_create("pixie",      pixie_thread,      (void *)(uintptr_t)700);
    thread_create("jack-frost", jack_frost_thread, (void *)(uintptr_t)1300);
    thread_create("herald",     herald_thread,     NULL);

    kprintf("threads     : the wheel turns, %ums quantum\n", 5 * (1000 / PIT_HZ));
    sched_dump();
    kprintf("\n");

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

    /* the boot thread's day job: echo the keyboard. the other threads
     * print over the top of us, which looks messy and is exactly the
     * point -- three threads sharing one console is the demo.
     *
     * hlt parks us until *any* interrupt lands (a key, or the timer
     * coming to preempt us). a properly blocking getchar needs a wait
     * queue, which arrives with the shell in m6 */
    for (;;) {
        int c = keyboard_getchar();
        if (c >= 0) {
            kprintf("%c", (char)c);
        } else {
            asm volatile ("hlt");
        }
    }
}
