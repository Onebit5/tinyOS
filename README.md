# tinyOS

a tiny 64-bit hobby kernel for x86_64, written in C, booted with [Limine](https://github.com/limine-bootloader/limine).

im building this to actually understand what happens between "power button" and "shell prompt". its not trying to be the next linux, its trying to fit in my head.

**version: 0.0.7** (milestone 5 — time passes and threads take turns. a pit ticking at 100hz, a preemptive round-robin scheduler, and pixie and jack-frost counting away in the background while the keyboard stays perfectly responsive)

## scope

roughly in order, this is where the project is going:

- [x] boot into 64-bit long mode via limine
- [x] serial (com1) logging
- [x] framebuffer console with its own font rendering
- [x] gdt/idt, real exception dumps instead of silent triple faults
- [x] ps/2 keyboard driver (interrupt driven, no polling)
- [x] physical page allocator + kmalloc heap on top
- [x] pit timer + preemptive round-robin scheduler with kernel threads
- [ ] a small interactive shell (help, mem, uptime, ps, the classics)
- [ ] ci that builds the iso and boot-tests it in qemu on every push

non-goals for now: usermode (maybe someday), networking, filesystems, being useful in any practical sense

## building

you need `gcc`, `nasm`, `make`, `xorriso` and `qemu` (any recentish versions). on fedora:

```
dnf install gcc nasm make xorriso qemu-system-x86
```

then:

```
make        # just the kernel elf
make iso    # bootable iso (fetches limine binaries on first run)
make run    # boot it in qemu
```

## layout

```
kernel/src/           main.c and friends
kernel/src/cpu/       gdt, idt, isr stubs, irq dispatch, the 8259 pic, port io
kernel/src/drivers/   serial, framebuffer console, the font, ps/2 keyboard, pit
kernel/src/lib/       kprintf, panic, string.h stuff
kernel/src/mm/        physical frame allocator, kernel heap
kernel/src/sched/     threads, the run queue, the context switch
kernel/linker.ld
tools/font2c.py       bdf -> C array converter for the console font
limine.conf           bootloader config
GNUmakefile
```

the console font is [spleen 8x16](https://github.com/fcambus/spleen) by frederic cambus (bsd 2-clause, see FONT-LICENSE), converted to a C array with `tools/font2c.py`.

## notes on memory

the kernel is still running on the page tables limine set up for us, on purpose. we use limine's hhdm (higher half direct map) to reach physical frames, which means the pmm can hand out any frame and we can immediately touch it without mapping anything ourselves. building our own pml4 is a later milestone. until that lands we also never reclaim the bootloader-reclaimable regions, because thats the memory our page tables live in.

the pmm and the heap are both written so their guts can be tested on a normal linux host: `pmm_init_from_map()` takes a memory map + hhdm offset instead of reaching for limine, so a test can fabricate one over a malloc'd arena. same trick as `keyboard_feed()`. host builds define `TINYOS_HOSTED`, which turns `irq_save()`/`irq_restore()` into no-ops (userspace isnt allowed to `cli`, and has nothing to lock out anyway).

## notes on threads

the context switch saves six callee-saved registers and a return address, and thats the entire parked state of a thread -- everything else the sysv abi already lets a function call clobber. rflags is deliberately *not* saved, because every way of resuming a thread restores it some other way: preempted threads come back through the `iretq` at the end of their interrupt, threads that yielded come back through `irq_restore()`, and brand new threads `sti` for themselves in the bootstrap. that last one is not optional -- a new thread arrives via `ret` with interrupts still off, and forgetting to enable them silently kills preemption for the whole system.

the timer irq sends its EOI *before* running the handler, which looks backwards. its because the scheduler can switch threads inside the timer handler and never return on that stack, and a freshly created thread has no half-finished interrupt frame to return through, so the EOI would never be sent and the pic would go quiet forever.

with preemption live, `kmalloc`/`kfree`/`pmm_alloc`/`pmm_free`/`kprintf` all disable interrupts for their duration. on one core thats the whole locking story: nobody can interrupt me means nobody else can run.

## changelog

- **0.0.7** — milestone 5. the pit ticks at 100hz on irq0 with a global tick counter and uptime. threads: kernel stacks from the pmm, a fabricated initial stack so a brand new thread can be "resumed" into existence, and a 16-instruction context switch in asm. preemptive round-robin scheduling on a 50ms quantum, blocking `sleep_ms()`, `thread_exit()` with a reaper that frees dead threads' stacks (from a different thread's stack, which is the only safe way). an idle thread that hlts so theres always somebody to hand the cpu to. the allocators and kprintf take interrupts down while they work, since a half-updated free list is nobodys friend. demo: pixie and jack-frost count at different rates, the herald says its piece and dies to give the reaper something to do, and typing still works throughout. the context switch is host-tested, including whether all six callee-saved registers actually survive a round trip.
- **0.0.6** — milestone 4. physical memory manager: parses limine's memory map (and prints it at boot), bitmap over every 4k frame, contiguous multi-page allocation with a rotating search hint, stats. kernel heap on top: first-fit free list, 16-byte aligned payloads, magic-guarded headers that catch double frees and wild pointers, address-ordered coalescing, grows by whole pages from the pmm. boot runs a self-test over both (8 frames + 5 heap blocks, pattern verified, freed out of order, books must balance) and panics if anything is off. tested on the host too, 25 assertions incl. draining ram dry and checking no frame is ever handed out twice.
- **0.0.5** — milestone 3. 8259 pic remapped to vectors 32-47 with spurious irq filtering, an irq_register() layer so drivers can claim lines, and a ps/2 keyboard driver: scancode set 1 -> ascii, shift + capslock state (they cancel, as the gods intended), e0 prefixes swallowed, all landing in a ring buffer. the scancode state machine is split from the irq handler and tested on the host (11 scenarios). boot now ends at a prompt that echoes thy keystrokes, live.
- **0.0.4** — milestone 2. our own gdt (tss slot reserved), idt with 256 macro-generated isr stubs, and an exception handler that prints the vector name, decoded page fault info (cr2 + error bits) and a full register dump before panicking. try `make clean && make FAULT_DEMO=1 run` to watch it catch a bad pointer with grace. the kernel also now boots, panics and (eventually) reboots with the appropriate persona social link ceremony. thou art I, and I am thou.
- **0.0.3** — milestone 1 done. kprintf (with actual tested number formatting), framebuffer console with the spleen 8x16 font, glyph blitting, scrolling, block cursor, and panic(). boot banner shows up on screen and serial at the same time. the temporary decimal-printer hack from 0.0.2 is gone, unmourned.
- **0.0.2** — com1 uart driver (polled, 115200 8n1, with loopback self test). framebuffer request to limine, boot info logged over serial, test pattern on screen. run `make run` and watch the serial chatter in your terminal.
- **0.0.1** — project scaffold. limine v9.x boots a stub kernel that halts politely. build system, linker script, license, this readme.
