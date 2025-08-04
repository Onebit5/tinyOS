# tinyOS

a tiny 64-bit hobby kernel for x86_64, written in C, booted with [Limine](https://github.com/limine-bootloader/limine).

im building this to actually understand what happens between "power button" and "shell prompt". its not trying to be the next linux, its trying to fit in my head.

**version: 0.0.5** (milestone 3 — the contract hath been sealed. the 8259 pic is remapped, irq1 flows, and whatever thou typest appears upon the screen, live, interrupt driven, no polling. the kernel now speaks fluent velvet room and this is permanent)

## scope

roughly in order, this is where the project is going:

- [x] boot into 64-bit long mode via limine
- [x] serial (com1) logging
- [x] framebuffer console with its own font rendering
- [x] gdt/idt, real exception dumps instead of silent triple faults
- [x] ps/2 keyboard driver (interrupt driven, no polling)
- [ ] physical page allocator + kmalloc heap on top
- [ ] pit timer + preemptive round-robin scheduler with kernel threads
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
kernel/src/drivers/   serial, framebuffer console, the font, ps/2 keyboard
kernel/src/lib/       kprintf, panic, string.h stuff
kernel/linker.ld
tools/font2c.py       bdf -> C array converter for the console font
limine.conf           bootloader config
GNUmakefile
```

the console font is [spleen 8x16](https://github.com/fcambus/spleen) by frederic cambus (bsd 2-clause, see FONT-LICENSE), converted to a C array with `tools/font2c.py`.

## changelog

- **0.0.5** — milestone 3. 8259 pic remapped to vectors 32-47 with spurious irq filtering, an irq_register() layer so drivers can claim lines, and a ps/2 keyboard driver: scancode set 1 -> ascii, shift + capslock state (they cancel, as the gods intended), e0 prefixes swallowed, all landing in a ring buffer. the scancode state machine is split from the irq handler and tested on the host (11 scenarios). boot now ends at a prompt that echoes thy keystrokes, live.
- **0.0.4** — milestone 2. our own gdt (tss slot reserved), idt with 256 macro-generated isr stubs, and an exception handler that prints the vector name, decoded page fault info (cr2 + error bits) and a full register dump before panicking. try `make clean && make FAULT_DEMO=1 run` to watch it catch a bad pointer with grace. the kernel also now boots, panics and (eventually) reboots with the appropriate persona social link ceremony. thou art I, and I am thou.
- **0.0.3** — milestone 1 done. kprintf (with actual tested number formatting), framebuffer console with the spleen 8x16 font, glyph blitting, scrolling, block cursor, and panic(). boot banner shows up on screen and serial at the same time. the temporary decimal-printer hack from 0.0.2 is gone, unmourned.
- **0.0.2** — com1 uart driver (polled, 115200 8n1, with loopback self test). framebuffer request to limine, boot info logged over serial, test pattern on screen. run `make run` and watch the serial chatter in your terminal.
- **0.0.1** — project scaffold. limine v9.x boots a stub kernel that halts politely. build system, linker script, license, this readme.
