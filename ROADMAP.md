# where this is going

0.1.0 is the point where programs stopped being a demo and became a
thing the kernel actually hosts: their own memory, their own privilege
level, reclaimed when they die.

what follows is a lot of small steps rather than one heroic one. each
line below should be a version on its own -- buildable, bootable, and
worth a paragraph in the changelog. the big one is at the bottom.

## 0.1.x — filling in what having processes implies

**0.1.1 processes, not just threads.** exit codes carried back to
whoever waited. a real process table with a parent. `ps` splitting
kernel threads from programs properly. `kill` learning to end a
program rather than refusing anything blocked -- which needs threads to
carry a back-pointer to the waitq they are parked on, the same gap that
makes `kill` timid today.

**0.1.2 more to ask for.** the syscall table is six calls wide. it
wants `open`/`read`/`close` against the ramdisk so a program can read a
file instead of being handed one; `getpid`; `spawn` and `wait` so a
program can start another. that last pair is what makes a userspace
shell possible at all.

**0.1.3 input that belongs to somebody.** right now the kernel shell
peeks at keys while a program runs and hopes. a foreground process
should *own* the input queue, with ctrl+c delivered to it rather than
handled on its behalf. that is the beginning of a controlling terminal.

**0.1.4 a userspace toolbox.** `cat`, `echo`, `uptime` as real programs
in `ramdisk/bin` rather than kernel commands. the kernel shell keeps
only what genuinely needs kernel access -- `vmm`, `bt`, `hexdump`, `ps`
-- and everything else moves out. the point is to find out which
commands were secretly using kernel internals.

**0.1.5 users.** a read-only `passwd` in the ramdisk, a login prompt, a
uid on each process, and syscalls that check it. worth saying: this
only means something because ring 3 exists -- without a boundary the
hardware defends, a "user" is a variable that says you are an admin.
persistence is not required for this; only *changing* users needs a
writable disk.

**0.1.6 a kernel that measures itself.** per-process cpu time, so `ps`
grows a cpu% column and the scheduler stops being theoretical. a `top`
that redraws. peak memory. how many syscalls of each kind.

**0.1.7 modern interrupt hardware.** acpi tables (limine hands over the
rsdp), then the lapic and ioapic in place of the 8259, and the lapic
timer in place of the pit. lateral on its own -- the same behaviour on
better hardware -- but it is the prerequisite for more than one cpu.

**0.1.8 knowing what is plugged in.** pci enumeration and an `lspci`.
small, satisfying, and the doorway to every real device driver.

**0.1.9 allocators worth the name.** the pmm is a linear bitmap scan; a
buddy allocator would make it logarithmic. a slab allocator for the
fixed-size things we allocate constantly (threads, address spaces). a
`kmalloc` that is not first-fit.

**0.1.10 a real filesystem, on a real disk.**

**0.1.11 our own bootloader.**