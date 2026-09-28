# the architecture boundary

everything under `x86_64/` assumes this machine. everything above this
directory is not supposed to.

that sentence was not true before 0.2.20. the kernel named registers all
over the tree: `mov %%cr3` in the address space code, `mov %%rbp` in the
backtrace, `hlt` in the scheduler, `pause` in three places, `sti` in two,
and nineteen files including an x86 header to get `irq_save` -- which is a
thing every architecture has and none of them spell the same way.

## drawn from the outside in

the headers in this directory are named for **what the rest of the kernel
wants**, not for what x86 happens to provide. that is the whole
difference between a boundary and a folder.

drawn the other way round, `arch/` would have ended up with a
`write_cr3()` -- an x86 instruction with a portable-looking name, which
is worse than the inline asm it replaced, because at least the inline asm
was honest about what it was.

so the four headers are:

| header | what it is for |
| --- | --- |
| `arch/irq.h` | may interrupts happen right now |
| `arch/cpu.h` | what this core can be told to do, and where it is |
| `arch/mmu.h` | what the hardware must be told about a page table |
| `arch/machine.h` | the box, rather than the processor: reset, power, a key |

each one picks its implementation with an `#if defined(__x86_64__)` and
an `#error` for everything else. the `#error` is the point: adding a
second architecture means the compiler lists exactly what is missing,
rather than the machine booting and being subtly wrong.

## what is deliberately still here and shouldn't be

the drivers that talk to ports -- the 8259, the pit, ps/2, the cmos
clock, the 8250 uart, pci configuration -- are still in `drivers/`. they
are as x86 as anything in this directory.

they are not moved because **a boundary drawn around drivers before there
is a second machine to draw it against is a guess.**

one of them stopped being a guess. an aarch64 port was written and then
removed (see ROADMAP.md), and while it existed it answered the question
for the serial driver: `drivers/serial.c` was a terminal with a chip
stuck to it. an 8250 is not an x86 chip -- *reaching* it through a port
space is -- so the escape-sequence machine stayed in `drivers/` and the
`outb`s went to `arch/x86_64/uart.c`.

the rest are still here because the port was removed before it could say
anything about them.

what is done in the meantime is making the leak *legible* and *counted*:
port io lives in `arch/x86_64/io.h`, so a file that does it visibly
includes an x86 header; `tools/checkarch.py` prints the list on every
build; and `tools/portable.py` catches the kind that has no include to
grep for.
