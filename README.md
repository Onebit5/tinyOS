# tinyOS

a tiny 64-bit hobby kernel for x86_64, written in C, booted with [Limine](https://github.com/limine-bootloader/limine).

im building this to actually understand what happens between "power button" and "shell prompt". its not trying to be the next linux, its trying to fit in my head.

**version: 0.0.2** (milestone 1, first half — the kernel talks over serial and owns the framebuffer. no text on screen yet, that needs a font renderer, but the test pattern proves the pixels are ours)

## scope

roughly in order, this is where the project is going:

- [x] boot into 64-bit long mode via limine
- [x] serial (com1) logging
- [ ] framebuffer console with its own font rendering
- [ ] gdt/idt, real exception dumps instead of silent triple faults
- [ ] ps/2 keyboard driver (interrupt driven, no polling)
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
kernel/src/cpu/       low level cpu stuff (port io for now)
kernel/src/drivers/   hardware drivers (serial for now)
kernel/linker.ld
limine.conf           bootloader config
GNUmakefile
```

## changelog

- **0.0.2** — com1 uart driver (polled, 115200 8n1, with loopback self test). framebuffer request to limine, boot info logged over serial, test pattern on screen. run `make run` and watch the serial chatter in your terminal.
- **0.0.1** — project scaffold. limine v9.x boots a stub kernel that halts politely. build system, linker script, license, this readme.
