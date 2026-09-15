#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "boot.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "cpu/interrupts.h"
#include "cpu/smp.h"
#include "cpu/tss.h"
#include "cpu/syscall.h"
#include "drivers/serial.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/input.h"
#include "drivers/pit.h"
#include "drivers/pci.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "fs/ramdisk.h"
#include "fs/disk.h"
#include "fs/vfs.h"
#include "sched/auth.h"
#include "sched/init.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "version.h"



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

    /* the buddy's whole point is that memory comes back *together*, not
     * merely back. after all that churn a big contiguous run must still
     * be there -- if the halves never merged, this is where I find out
     * rather than the first time something large is asked for */
    uint64_t big = pmm_alloc_pages(512);
    if (big == 0) {
        panic("selftest: 2 MiB contiguous is already gone. blocks are not merging");
    }
    pmm_free_pages(big, 512);
}

/* which console the calling thread belongs to. the console driver asks
 * this on every write, because output belongs to its writer rather than
 * to whichever console is being looked at -- a shell on console 2
 * printing while console 1 is displayed must not scribble over console
 * 1, and that is the whole difference between four consoles and one
 * console with four names */
static unsigned which_console(void) {
    struct thread *t = sched_current();
    return (t != NULL) ? t->console : console_active();
}

/* and whether it should go down the wire as well. the serial line shows
 * whichever console is on the screen -- it is a second window onto one
 * seat rather than a fifth console, because the keyboard already gives
 * its keys to the console being looked at and anything else would mean
 * typing at one shell while reading another */
static bool writing_to_the_shown_console(void) {
    return which_console() == console_active();
}

void kmain(const struct ph_handoff *handoff) {
    boot_take_handoff(handoff);

    /* boot_take_handoff has already refused to come back if the struct
     * is not one of philemon's, which is the only check worth making
     * this early -- there is no console and no serial to complain to */

    serial_init();
    gdt_init();
    idt_init();
    pic_init();
    keyboard_init();
    mouse_init();
    serial_input_init();

    const struct ph_framebuffer *fb = &boot_handoff()->fb;
    if (fb->width == 0) {
        kprintf("no video mode. serial only, which is enough to see by\n");
    } else {
        console_init(fb);
        if (!console_ready()) {
            kprintf("console refused %u bpp, serial only from here\n", fb->bpp);
        }
    }

    /* from here until the shell is ready, everything goes to serial and
     * the log but not to the screen. all of it is worth having when
     * something breaks and none of it is worth reading when it doesnt */
    kprintf_to_console(false);

    kprintf("tinyOS v%s\n", VERSION);
    kprintf("framebuffer : %ux%u @ %u bpp, pitch %lu bytes, at %016lx\n",
            fb->width, fb->height, fb->bpp, fb->pitch, fb->address);
    kprintf("font        : spleen 8x16 (bsd 2-clause)\n");
    kprintf("gdt         : loaded, tss slot reserved for later\n");
    kprintf("idt         : 256 gates armed, exceptions get caught now\n");
    kprintf("pic         : 8259 remapped to vectors 32-47, ghosts filtered\n");
    kprintf("keyboard    : ps/2 on irq1, me layout, listening\n");
    kprintf("serial in   : com1 on irq4, the shell answers over the wire too\n");
    kprintf("timer       : pit channel 0 at %u hz, %ums per tick\n",
            PIT_HZ, 1000 / PIT_HZ);
    kprintf("kernel      : loaded at %p\n\n", (void *)kmain);

    pmm_init();
    memory_selftest();
    kprintf("  -> selftest: frames and heap blocks round-tripped, blocks merge, "
            "books balance\n");
    kprintf("memory      : %lu MiB free of %lu MiB, heap warmed to %lu KiB\n\n",
            pmm_free_bytes() / (1024 * 1024),
            pmm_total_bytes() / (1024 * 1024),
            kheap_total_bytes() / 1024);

    kprintf("building my own page tables:\n");
    vmm_init();

    /* needs the pmm for its stacks, so it waits until now */
    tss_init();
    idt_set_ist(8, IST_DOUBLE_FAULT);
    kprintf("  -> tss loaded, double faults land on their own stack\n");

    syscall_init();
    kprintf("  -> syscall/sysret armed, ring 3 has a way in\n\n");

    /* before the shell reclaims the loader's memory, since the ramdisk
     * I read this out of is sitting in it */
    pci_scan();
    kprintf("pci        : %zu devices on the bus\n", pci_count());

    /* the first time anything is done with a device I found rather
     * than merely counted. a machine with no disk carries on exactly as
     * it did before there was any of this */
    if (disk_mount()) {
        uint64_t used = 0, total = 0;
        disk_usage(&used, &total);
        kprintf("disk       : %s, %lu MiB, fat32 \"%s\" mounted at /\n",
                disk_model(), disk_bytes() / (1024 * 1024), disk_label());
        kprintf("             %lu KiB used of %lu MiB, ramdisk at %s\n",
                used / 1024, total / (1024 * 1024), VFS_BOOT);
    } else {
        kprintf("disk       : none found. %s is all there is, "
                "which is enough\n", VFS_BOOT);
    }

    ramdisk_init();
    auth_init();
    kprintf("\n");

    /* from here on this function is a thread like any other */
    sched_init();
    pit_init();

    /* and now, if the firmware will say where they are, move every
     * interrupt off the 8259 and onto the apics. this is lateral on its
     * own -- the same interrupts by a better road -- and it is the
     * thing a second cpu would need. if acpi tells me nothing I stay
     * on the old chip, which works perfectly well */
    if (!interrupts_use_apic()) {
        kprintf("interrupts : staying on the 8259 and the pit\n");
    }

    /* and then wake everything else this machine has. they climb out
     * into long mode, say which core they are, and halt -- giving them
     * work needs locks that do not exist yet. before the shell reclaims
     * the loader's memory, because the page they start on is in it */
    smp_init(interrupts_acpi(), interrupts_timer_rate());

    /* the console driver has to be able to ask who is writing before
     * anything writes. set before the shells exist, because the first
     * thing they do is print */
    console_set_owner_hook(which_console);
    kprintf_serial_filter(writing_to_the_shown_console);

    /* the screen is the user's from here.
     *
     * this used to be done by the first shell, once it had finished
     * reclaiming the loader's memory -- which was fine while it was the
     * only shell. with four of them it is a race: the other three print
     * their greeting and their login prompt as soon as they run, and
     * anything printed before this line goes nowhere at all. the effect
     * was three consoles sitting at an invisible "username:" waiting
     * for an answer to a question nobody had been asked.
     *
     * so it happens here, before any of them exists. nothing runs until
     * the sti at the end of this function anyway */
    kprintf_to_console(true);

    kprintf("threads     : the wheel turns, %ums quantum\n",
            5 * (1000 / PIT_HZ));

    /* and this is the last thing kmain decides.
     *
     * up to 0.2.19 the four shells and the disk flusher were created
     * right here, because there was nothing else that could create
     * them. that is fine exactly once and answers nothing afterwards:
     * what order things come up in, what happens when one of them dies,
     * who owns a process whose parent has gone, and what shutting down
     * means. all four of those are one job, and the job has a name.
     *
     * so kmain starts one thing now, and that thing starts the machine */
    if (!init_boot()) {
        panic("no init. there is nobody to bring the machine up");
    }

    asm volatile ("sti");


    /* the boot thread's work is finished. it has to actually leave --
     * its stack is the loader's, sitting in the memory init is about to
     * reclaim, and you cannot free the ground you are standing on */
    thread_exit(0);
}
