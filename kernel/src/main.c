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
#include "sched/sched.h"
#include "sched/thread.h"
#include "shell/shell.h"
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

/* what the user actually sees at boot: the name, the contract, and
 * whatever /boot/welcome.txt has to say -- named absolutely on purpose,
 * so that what the machine says about itself at boot cannot be changed
 * by whatever happens to be sitting on the data disk. everything the drivers had to report
 * went to serial and is still there under `dmesg` */
static void greet(void) {
    console_clear();

    console_set_colors(0x45e653, 0x101018);
    kprintf("tinyOS v%s\n\n", VERSION);

    console_set_colors(0x7b8ce0, 0x101018);
    kprintf("Thou art I... And I am thou...\n");
    kprintf("Thou hast established a new bond...\n\n");
    kprintf("Thou shalt be blessed when creating\n");
    kprintf("Personas of the Computer's Arcana...\n\n");

    console_set_colors(0xc8c8d0, 0x101018);

    struct ramdisk_file f;
    if (ramdisk_open("welcome.txt", &f)) {
        const char *p = f.data;
        for (uint64_t i = 0; i < f.size; i++) {
            kprintf("%c", p[i]);
        }
        kprintf("\n");
    }
}

/* the shell runs here rather than on the boot thread, because this
 * stack came from the pmm. that is what lets the boot thread walk away
 * from the loader's stack and lets me hand the loader's memory back */
static void shell_thread(void *arg) {
    (void)arg;

    uint64_t gained = pmm_reclaim_bootloader();
    kprintf("reclaimed %lu KiB of bootloader memory (%lu MiB usable now)\n",
            gained / 1024, pmm_total_bytes() / (1024 * 1024));
    kprintf("boot complete, handing the screen to the shell\n\n");

    /* the screen is the user's from here */
    kprintf_to_console(true);
    greet();

    shell_run();
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

    if (thread_create("shell", shell_thread, NULL) == NULL) {
        panic("no memory for a shell. there is nobody left to talk to");
    }

    kprintf("threads     : the wheel turns, %ums quantum\n\n",
            5 * (1000 / PIT_HZ));

    asm volatile ("sti");


    /* the boot thread's work is finished. it has to actually leave --
     * its stack is the loader's, sitting in the memory the shell is about
     * to reclaim, and you cannot free the ground you are standing on */
    thread_exit(0);
}
