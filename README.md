# tinyOS

![ci](https://github.com/USERNAME/tinyOS/actions/workflows/ci.yml/badge.svg)

a tiny 64-bit hobby kernel for x86_64, written in C, booted by a bootloader of its own.

im building this to actually understand what happens between "power button" and "shell prompt". its not trying to be the next linux, its trying to fit in my head.

**version: 0.2.5** (**arguments worth parsing.** each program declares once what it takes, and that declaration is the only description of it there is)

## what it does

- [x] boot into 64-bit long mode
- [x] serial (com1) logging, and serial input too -- the shell answers either way
- [x] framebuffer console with its own font rendering
- [x] gdt/idt, real exception dumps instead of silent triple faults
- [x] ps/2 keyboard driver (interrupt driven, no polling)
- [x] physical page allocator + kmalloc heap on top
- [x] pit timer + preemptive round-robin scheduler with kernel threads
- [x] an interactive shell with line editing, history and tab completion
- [x] my own page tables: W^X, NX, guard pages under thread stacks
- [x] a tss with an IST, so a stack overflow reports instead of rebooting
- [x] symbolized backtraces on panic
- [x] a read-only ramdisk, unpacked from a tar the loader hands me at boot
- [x] ring 3, `syscall`/`sysret`, and an elf loader -- it runs programs
- [x] an address space per program, reclaimed when it dies
- [x] a process table: pids, parents, and exit codes that outlive the thread
- [x] file descriptors, and `spawn`/`wait` -- a program can start a program
- [x] a controlling terminal: a foreground process, and ctrl+c delivered to it
- [x] arguments, and a toolbox that lives outside the kernel
- [x] users: a login, a uid per process, and files a guest may not read
- [x] cpu accounting, peak memory, syscall counts, and a live `top`
- [x] acpi, the lapic and io apic, and a timer that is part of the cpu
- [x] pci enumeration, and an `lspci` that says what this machine is
- [x] a buddy page allocator, slab caches, and a heap that stopped searching
- [x] a sata driver and fat32: files on a real disk, that survive a reboot
- [x] one namespace over both, and a machine that boots with neither missing
- [x] philemon: my own bootloader, and now the only one on the disk
- [x] the other processors, woken and accounted for
- [x] real locks, in ranked order, and a test that can see a race
- [x] every core running threads, and tlb shootdown between them
- [x] a working directory, `cd`, `pwd`, and directories that can be made
- [x] a search path, and `help` that stopped dividing the world in two
- [x] arguments parsed in one place, declared once per program
- [x] ci that builds the iso, boot-tests it in qemu, and types at the shell

where it goes next is [ROADMAP.md](ROADMAP.md): eleven more steps, ending in a
filesystem on a real disk and a bootloader of my own.

still a non-goal: networking, and being useful in any practical sense.

## building

you need `gcc`, `nasm`, `make`, `xorriso` and `qemu` (any recentish versions). on fedora:

```
dnf install gcc nasm make xorriso qemu-system-x86
```

then:

```
make        # just the kernel elf
make iso    # bootable iso (fetches the loader binaries on first run)
make run    # boot it in qemu
make test   # run the host test suites (no qemu needed, takes a second)
make boottest   # boot the iso and drive the shell over serial
```

[TESTING.md](TESTING.md) has what to check by hand, version by version -- a
two-minute smoke test, then the specific thing each version introduced, so a
regression can be bisected to the milestone that owns it.

`make run` gives you a qemu window *and* a serial console in your terminal -- and since com1 is wired into the input queue, you can type at either one. the shell cannot tell the difference.

## layout

```
kernel/src/           main.c and friends
kernel/src/cpu/       gdt, idt, isr stubs, irq dispatch, the 8259 pic, port io
kernel/src/drivers/   serial, framebuffer console, the font, ps/2 keyboard, pit
kernel/src/lib/       kprintf, panic, string.h stuff
kernel/src/mm/        physical frame allocator, kernel heap, page tables
kernel/src/sched/     threads, the run queue, the context switch
kernel/src/shell/     the velvet room terminal
kernel/linker.ld      higher half layout + the section symbols the vmm maps by
tests/                host test suites, run with `make test`
tools/                font2c.py (bdf -> C array), boottest.sh
the loader.conf           bootloader config
GNUmakefile
```

the console font is [spleen 8x16](https://github.com/fcambus/spleen) by frederic cambus (bsd 2-clause, see FONT-LICENSE), converted to a C array with `tools/font2c.py`.

## the shell

boot lands you at a `velvet>` prompt. commands:

```
help      list what thou may command
clear     wipe the screen clean
echo      say something back
ls        what the ramdisk carries
run       give a program the outer ring
dmesg     everything boot said while you werent looking
cat       read a file aloud
arcana    the rank of this bond, and its making
persona   the face this machine wears
mem       frames and heap, honestly counted
uptime    how long since the bond was formed
ps        the threads that walk this realm
bt        who called whom to get here
date      what the battery-backed clock believes
hexdump   look at memory, safely
kill      end a thread by id
history   what thou hast said before
time      how long a command takes
summon    call forth a persona thread
vmm       what the page tables say about an address
crash     tempt fate with a wild pointer
smash     run off the end of the stack on purpose
reboot    sever the bond and begin anew
poweroff  let the velvet room fade
```

`vmm` with no argument points at one thing of each kind -- code, a string constant, the heap, your stack, and an address nobody lives at -- so you can read the permission column and see W^X actually holding. give it a hex address to look that up instead.

line editing, as close to readline as a hobby kernel needs:

| | |
|---|---|
| left / right, ctrl+b / ctrl+f | move the cursor, and you can type in the middle |
| ctrl+a / ctrl+e | start and end of line |
| backspace, del / ctrl+d | delete behind and ahead |
| ctrl+w / ctrl+u / ctrl+k | kill a word, the line, or to the end |
| up / down | the last 16 commands |
| tab | complete a command name, or a filename after `cat`/`run` |
| ctrl+l | wipe the screen, keeping the line you were typing |
| ctrl+c | abandon the line, and recall any running personas |

adjacent duplicates and empty lines dont make it into the history.

all of that rests on one small change: the console used to treat `\b` as "move left and erase", which made `"\b \b"` work by accident. now it moves only, the way every real terminal does -- so `"\b \b"` still erases *and* a bare `\b` is non-destructive cursor movement that behaves identically on the framebuffer and down the serial line. the console grew a shadow buffer of what character is in each cell to go with it, because a block cursor sitting *on* a character has to put that character back when it moves away, and a framebuffer cannot tell you what used to be there.

`hexdump` asks the page tables whether an address is mapped before reading it, so a typo prints `<not mapped>` instead of panicking. `kill` works on a thread in any state, including one blocked on a waitq -- it takes it off that queue first, which is what the back-pointer added in 0.1.1 is for.

`summon pixie` and `summon jack-frost` spawn real kernel threads that count in the background while you keep typing -- that is the whole scheduler demo in one command. they speak eight times and then depart, which also gives the reaper something to clean up (watch `ps` before and after). they print over the top of your prompt while they run, which looks messy and is entirely honest: three threads are sharing one console and nobody is arbitrating.

cancelling them with ctrl+c is cooperative, not forceful -- I have no signals and no safe way to yank a sleeping thread off the run queue, so a persona notices it has been recalled the next time it wakes up. that can be up to one sleep period later.

`crash` dereferences `0xdeadbeef` on purpose, which page faults inside the shell thread and gets you the full m2 exception report -- decoded fault reason, cr2, every register, then the panic. the machine is dead at that point, but the panic handler polls the 8042 directly (interrupts are never coming back, so the keyboard driver is no help) and any keypress resets the box. it ignores key *releases*, otherwise letting go of the enter key you used to type `crash` would reboot instantly.

## backtraces

a panic used to give you `rip=ffffffff800029dc` and leave you to run `addr2line`. now it prints this:

```
call trace:
  0xffffffff80007a1c  cmd_smash+0x1c
  0xffffffff800070f4  run_line+0x40
  0xffffffff80007042  shell_run+0x11a
```

the kernel carries its own symbol table, generated from `nm` output at build time by `tools/gensyms.py` and baked into a `.ksyms` section -- the same trick `tools/font2c.py` plays with the console font. lookup is a binary search for the last symbol at or below the address.

there is an obvious chicken and egg here: the table records addresses, and linking the table in changes addresses. the way out is placement. `.ksyms` sits *after* `.text` in the linker script, so folding it in shifts `.data` around but cannot move a single function. that makes a plain two-pass build correct rather than something I have to iterate to a fixed point. and because "cannot" is doing a lot of work in that sentence, the build runs `gensyms.py --check` afterwards, which re-reads the finished binary and fails loudly if any symbol the table describes has moved.

the walker itself assumes frame pointers, so the kernel builds with `-fno-omit-frame-pointer`. it is normally called from a panic, which means nothing in it may fault -- a page fault inside the backtrace printer would bury the real bug under a second one. so every frame pointer is checked against the page tables with `vmm_translate` before being dereferenced, and the walk stops the moment the chain stops making sense (a caller's frame must be at a higher address, stacks growing down as they do).

`bt` in the shell prints a trace with nothing on fire, which is a good way to see it work.

## ring 3

`run bin/hello` loads an elf out of the ramdisk and gives it the outer ring. it cannot touch a port, cannot read the kernel, and cannot see any memory but its own -- the only thing it can do to the world is ask, through `syscall`, and be answered.

`ls` prints paths exactly as `cat` and `run` accept them -- tar stores `./bin/hello`, and showing that verbatim tells you to type something that then does not work. there is no path search, so `run hello` will not find `bin/hello`; instead of adding magic that surprises you later, a miss says which file you probably meant.

`user/` is a whole tiny userland: a freestanding program with no libc, six inline syscall stubs, and its own linker script putting it at `0x400000` in the low half where the kernel's mappings can never reach.

three things about this were easy to get wrong and interesting to get right:

**two calling conventions meet at the entry stub, and they are not the same one.** ring 3 hands over `nr` in rax and arguments in rdi/rsi/rdx/r10/r8; sysv C wants them in rdi/rsi/rdx/rcx/r8/r9, and the dispatcher's *first* argument is the number, so everything shifts one place right to make room. get that wrong and the kernel reads an argument as the call number, which looks exactly like a program asking for syscall 4198426. the shuffle also has to be written in an order where every register is read before anything overwrites it.

**ring 3 gets every register back but three.** the syscall instruction destroys rcx and r11, and rax carries the result -- everything else the user compiled against the assumption that it survives. so the entry stub has to preserve the caller-saved registers itself, because the C dispatcher is free to clobber them and the argument shuffle certainly does. miss that and kernel values leak into a program that will use them as pointers, which surfaces as a page fault in userspace at an address that means nothing to anyone. (rbx, rbp and r12-r15 need no saving there: `syscall_dispatch` is an ordinary C function and the abi makes those its problem.)

**`syscall` does not switch stacks.** it puts the return address in rcx, the flags in r11, loads cs and rip from MSRs, and that is all. you arrive in ring 0 *standing on the user's stack*, which is as alarming as it sounds. the entry stub's first job is to get off it. it can do that with a global only because `SFMASK` clears IF, so I arrive with interrupts off and nothing can preempt me in the three instructions before the user's rsp is safely parked on a kernel stack.

**the gdt layout is not mine to choose.** `sysret` computes `CS = STAR[63:48] + 16` and `SS = STAR[63:48] + 8`, so user data has to sit eight bytes below user code or returning to ring 3 lands nowhere. there is a test asserting that relationship, because it is the sort of thing a tidy-up would quietly break.

**the user bit is ANDed down the whole chain.** a leaf marked `PTE_USER` under intermediate tables that are not is unreachable from ring 3, and it looks completely correct in any dump you care to print. the vmm now grants the bit at every level on the way to a user mapping, and widens tables that were built for a kernel mapping and later find themselves on the path to a user one. kernel leaves stay supervisor-only regardless. that one has a test that walks all four levels by hand.

### what a program may ask for

eleven syscalls, and no libc out there to satisfy -- only what a program in this kernel could actually want:

```
exit(code)              write(fd, buf, len)     read(fd, buf, len)
uptime()                yield()                 sleep(ms)
open(path) -> fd        close(fd)               getpid()
spawn(path) -> pid      wait(pid, &code)
```

`read` and `write` take a descriptor first, the way they do everywhere. 0, 1 and 2 are the console; anything from 3 up is a file, and a descriptor is a bookmark into the ramdisk rather than a copy of it -- the archive is already in memory and read-only, so there is nothing to allocate and nothing to free. they belong to the process, so they close when it ends.

every pointer a program hands over is checked against **that program's** page tables, not the kernel's. the kernel's have no mapping for a program's memory at all, so checking there says no to every pointer that was ever going to be valid -- which is precisely what 0.1.0 shipped and nobody noticed until a program ran. a refusal is now logged rather than returned in silence, because a program that ignores an error and prints nothing is a miserable thing to debug.

`spawn` and `wait` are the pair that matters. up to 0.1.1 only the kernel shell could start a program; now a program can, and can be told how its child went -- which is what makes a shell in ring 3 possible, and what 0.1.4 is for. a process may only wait for its own children, or one could collect another's and send the exit code to the wrong place.

`bin/reader` opens `motd.txt` and reads it in 32-byte bites to show the descriptor keeping its place; `bin/parent` spawns `bin/fail`, waits, and passes on the 42 it gets back -- a number that crossed two address spaces and outlived the thread that produced it.

### arguments worth parsing

every program used to read `argv` by hand and no two of them agreed.
some took a flag anywhere, some only first, most took none at all and
silently treated `-v` as a filename. that is the kind of inconsistency
nobody notices until they trust it.

there is one parser now, and a program **declares what it takes**:

```c
static const struct opt cat_opts[] = {
    { 'v', "verbose", false, "name each file and its size before its contents" },
    { 'n', "number",  false, "number the lines" },
};
```

that declaration is the only description of the program there is. the
parser reads it, and so does anything that has to explain the program to
somebody -- which is what stops usage text from drifting away from what
the code actually does, the usual fate of usage text. 0.2.6 is that
second reader.

what is understood is what everybody already expects: `-v`, `--verbose`,
`-abc` for three at once, a value as the next word or stuck on or after
an `=`, and `--` to say that everything after it is a filename however
much it looks like an option. that last one is not a nicety: it is the
only way to open a file whose name begins with a dash, and there is a
test that says so.

`--help` and `-h` are noticed by the parser and *not acted on*. what to
print is the next version's business, and a parser should not decide to
write things.

`cat -v` names each file before its contents, `cat -n` numbers the
lines, `ls -1` prints one name per line and nothing else, `echo -n`
leaves the newline off, and `write -t` replaces a file rather than
adding to it.

### commands that are just commands

typing a program by its name has worked since 0.1.4, but only by a trick:
the shell stuck `bin/` on the front and asked the *ramdisk* directly,
going around the vfs entirely. so a program on the disk could never be a
command, and `./thing` meant nothing at all.

there is a search path now -- `/bin`, then `/boot/bin` -- looked up
through the vfs like everything else, so a disk can supply a command and
a name earlier on the path hides one later. **the working directory is
deliberately not on it.** a name typed on its own should mean the same
thing wherever you happen to be standing, and a program left lying in a
directory should not quietly become a verb there. say `./thing` if that
is what you mean: anything with a slash in it is a path, read from where
you are and taken exactly as written.

`help` is one list now. whether something runs inside the kernel or out
in ring 3 with an address space of its own is a fact about how it was
built, not about how it is used -- so it is a dot in the margin rather
than a heading to look under. completion offers both, for the same
reason: a completion that knew only the builtins would be redrawing the
line the rest of this version spent its time rubbing out.

one asymmetry had to go to make it work. the ramdisk is a flat archive
-- it holds a name like `bin/hello`, not a directory called `bin` with a
`hello` in it -- so `/boot/bin/hello` could be *opened* while `/boot/bin`
could not be *listed*. a namespace that answers two different questions
two different ways is one you cannot build on, so listing a directory
inside it now picks the names that begin with it and shows what follows.

### somewhere to stand

until now nothing in this kernel knew where it was. every name was
absolute or nearly so, and `..` was thrown away by the resolver without
comment -- there was nowhere to go back *to*.

a process has a working directory now. a name is read relative to it
unless it begins with a slash, and the whole thing is flattened into one
absolute path before anybody goes looking on a disk: `.` means here,
`..` means back one, repeated slashes mean nothing, and **`..` from the
root stays at the root**. that last rule is the one that has to be
right, because a path able to climb above `/` is a path that can name
anything at all. it has a test of its own, and so does refusing a path
that will not fit rather than truncating it -- a truncated path is a
different path, and quietly acting on one is how you delete the wrong
thing.

resolution happens at the syscall boundary, once, so that no filesystem
below ever sees a name that means different things to different callers.
a process inherits the directory of whoever started it, which is what
makes `cd` somewhere and then running something behave the way anybody
would expect.

`cd` and `pwd` are shell builtins and have to be: a program runs as its
own process with its own working directory, so a `cd` that was a program
would change where *it* was standing and then exit.

two things fell out of it that were quietly missing before. **the mount
points could not describe themselves** -- `/` is where the mounts hang
from and `/boot` *is* the ramdisk, so neither is on any filesystem, and
nothing could say they were directories. you cannot stand somewhere that
nothing will admit exists. and **the ramdisk fallback had to grow**: it
used to apply only to names typed without a slash, but now every name
arrives absolute, so without extending it a machine with no disk could
reach no program, no passwd and no file at all.

`mkdir` and `rmdir` go with it, down to the filesystem: a new directory
is born with the two entries every directory has, and an empty one is
unmade by striking out its record and letting its clusters go. only an
empty one -- taking a whole tree away is a different operation and ought
to look like one at the point of asking.

### a scheduler on every core

0.2.0 woke the other processors and gave them nothing to do. 0.2.1 made
the locks real. this is the version where they start doing the work.

**one run queue, not one each.** the roadmap said a queue per core with
work moved between them; I built a single ring that every core picks
from, and that is a deliberate difference. at four cores the lock is not
the bottleneck, and an idle core taking whatever happens to be ready
*is* load balancing -- without the migration machinery that per-core
queues then need in order to undo what splitting them apart did. worth
revisiting when there is evidence the lock is the thing in the way,
which there is not.

what genuinely cannot be shared is per core: which thread it is running,
the idle thread it falls back to, how much of a slice is left, and its
own task state segment.

**a core learns its own name from the task register.** every core loads
a different tss selector, so the register the processor is already
holding *is* a core's identity -- two cycles to read, and it needs
nothing in memory to have been reached first. that last part is what
makes it usable: this question gets asked on every lock and every
context switch, and the obvious alternative -- reading the local apic --
is an uncached access to a device thousands of times a second, to learn
something the cpu already knew.

**and the syscall path was per-machine where it had to be per core.**
`syscall` is fast because it does almost nothing: it does not even
change the stack pointer, so the kernel arrives in ring 0 standing on
the *user's* stack and the first job is to get off it. that swap used
two globals, and the stub said why that was safe -- SFMASK clears the
interrupt flag, so nothing can preempt those three instructions.

the reasoning was airtight and is now wrong: it says nothing about
another core. with four of them, a global kernel stack pointer means a
thread can `syscall` its way onto a stack another core is standing on.
so those two words are per core now, reached through `gs` -- which can
be read without clobbering a single register, and that matters here more
than anywhere: `rax` holds the call number, `rcx` and `r11` hold what
`sysret` needs, everything else holds arguments, and there is no stack
yet to save anything on.

there is no `swapgs`: `gs` simply names the current core in both rings,
always, so there is no parity to keep in step and nothing to get wrong
when a thread migrates mid-call.

that only works if nothing zeroes the base -- and the trip into ring 3
was doing exactly that. in 64-bit mode the `gs` base does not come from
the descriptor table at all; it comes from an msr. loading any real
selector into `gs` overwrites that base with the descriptor's, which is
zero. `enter_usermode` was loading a user selector into `gs` along with
the others, so the instant any program reached ring 3 its core stopped
knowing which core it was, and the program's first system call wrote
through a base of zero and faulted inside the kernel.

it is left alone now. nothing in ring 3 reads `gs`, and `iretq` nulls
the selector by itself when it drops privilege.

`star`, `lstar`, `sfmask` and the gs bases are every one of them per
core too, and only the boot core had them. a thread scheduled onto any
other executed `syscall` and jumped to address zero -- in ring 0, on the
user's stack.

**three things had to change that are not about scheduling at all:**

- **the clock.** every core has its own timer and all of them arrive at
  the same tick handler. if all four counted, an hour would pass in
  fifteen minutes and every sleep in the system would end early. the
  boot core keeps time; the rest only schedule.
- **the reaper.** a thread marked dead may still be *running* -- `kill`
  can mark one that is on another core this instant -- so freeing its
  stack is not a race, it is pulling the floor out from under a
  processor. dead threads are now unlinked under the lock and freed
  outside it, from the idle loop, and only once their core has let go.
- **the idle thread.** a core creating its own idle thread puts it in
  the ring before claiming it, and in that gap another core could pick
  it up -- two processors on one stack. it goes in parked.

**and tlb shootdown**, because when one core changes a page table the
others are still holding the old translation and nothing in the hardware
tells them. it takes an interrupt to each and a wait until every one has
answered -- the wait being the part that matters, since returning early
means carrying on while another core still uses a mapping you have
already taken away.

the wait is bounded, and deliberately far shorter than the spinlock's
own patience: a core that has not answered by then is one that cannot,
and waiting past the point where somebody else declares *you* stuck
helps nobody.

`ps` now says which core each thread is on -- and a thread that is
merely ready is on none of them. `cpus` says what each core is running.

### locks worth the name

this kernel spent twelve versions using `cli` as mutual exclusion. that
worked, and for a real reason: with one core, the only thing that could
interrupt a critical section was an interrupt, and turning them off
stopped it. it was not a shortcut. it was the correct answer to the
question being asked.

the question changed in 0.2.0. turning interrupts off on *this* core
says nothing whatsoever about a thread running on *that* one. thirty-nine
places across twelve files went from correct to wrong without a single
one of them being edited.

so 0.2.1 is an audit rather than a feature. every one of those sites is
now a real lock -- and the primitive was deliberately shaped like the
thing it replaces:

```c
uint64_t flags = spin_lock_irq(&pmm_lock);
...
spin_unlock_irq(&pmm_lock, flags);
```

that is the same shape `irq_save`/`irq_restore` had, which is the point:
an audit of thirty-nine call sites is worth doing where every change
looks identical and any that does not stands out. it does both halves at
once, because a critical section that needs protecting from another core
almost always also needs protecting from this core's own interrupt
handlers, and needs both or neither.

**the order they are taken in is checked, not hoped for.** two locks
taken in opposite orders by two cores is a machine that stops -- no
fault, nothing printed, nothing to debug. so every lock declares a rank,
and the ranks are not invented: they are read off the call graph. the
tty calls the scheduler, the scheduler reaches into the process table,
all of them allocate, and anything may print. nothing goes back the
other way. a lock may only be taken while holding lower-ranked ones.

for now that check *complains* rather than panics, and the difference is
the whole reason for doing this a version early: with one core running
kernel code, a wrong order cannot actually deadlock anything, so a rank
I got wrong should cost a line of text rather than a working machine. it
becomes fatal in 0.2.2, when it can bite.

**and there is finally a test that can see a race.** every other suite
runs one thread through code that used to assume one core, and passes
whether or not that code is safe -- because with one thread it *is*
safe. `test_locks` runs eight real threads through the real allocators
at once. on the host the interrupt half of the lock is a no-op and the
lock half is entirely real, so what gets exercised is precisely the half
0.2.0 made necessary.

it carries its own control: the same counter is incremented with the
lock and without it, and the test asserts the unguarded one **comes out
wrong**. if it did not, the threads never really overlapped and nothing
else in the file proved anything. that check runs several rounds and
needs to lose an update only once -- a race is not obliged to happen on
any particular try, and a flaky test in the one file whose job is
catching races would be worse than not having it.

and it earned itself on the first boot. the lock caught four things the
audit had walked straight past, all the same shape: **a lock is a
property of the machine and must never be held across a context switch,
where `cli` was a property of the thread and rode through one
harmlessly.** a new thread was starting with the scheduler's lock held
and its release sitting on a stack it would never return to; the
blocking reader held the keyboard's lock while asleep, so the interrupt
that would have woken it spun on that same lock; the timer tick walked
the run queue with nothing held at all; and waking a thread took the
scheduler's lock twice over. every one of them was correct on one core
and none of them survived having a second.

`locks` shows what guards what, and how often anything has had to wait.

### waking the other cores

the firmware has been telling me how many processors this machine has
since 0.1.7 and I have been using exactly one of them. the rest sit in
reset, and the only thing in the world that can bring one out is an
interrupt sent from another core's local apic -- which is why none of
this could be attempted before there was a local apic to send it from.

a core that wakes does not resume. it **starts**: real mode, sixteen
bits, at a page below one megabyte, knowing nothing about the last forty
years. the startup message carries a vector and the vector is a *page
number*, so vector 8 means the core begins executing at 0x8000 with no
stack, no page tables, and no idea what year it is. so there has to be a
page of code sitting there to catch it, making the same climb into long
mode that philemon makes at boot -- in a quarter of the space, on a
processor that is not the first.

two details are worth knowing because they are where this goes wrong:

**the page tables it climbs on cannot be the kernel's.** the kernel maps
none of low memory, and the trampoline is down in low memory -- so the
instruction after paging comes on would be unreachable. it climbs on a
copy with the first two megabytes identity-mapped, and the C it lands in
switches to the real ones as its first act, once it is executing at an
address the kernel knows about.

**it is asked twice.** the sequence is fixed by the manual: assert INIT,
wait, then send STARTUP, then send STARTUP again. the second is not
superstition -- some processors miss the first, and sending it to one
that already started is harmless because it is no longer listening.

**what they do when they arrive is nothing.** each core reads its own
apic id and writes it down, which is proof it really executed my code on
that processor -- it is the one thing a core cannot be wrong about or
fake -- and then halts. `cpus` shows the result.

that restraint is the whole point of stopping here. there are **39
`irq_save` pairs across twelve files** in this kernel, and every one of
them is a lie the moment a second core runs kernel code: turning
interrupts off *here* says nothing about a thread running *there*. the
fat32 driver has a single 512-byte scratch buffer shared by every call
into it. giving these cores work now would not be a slow answer, it
would be a corrupt one. 0.2.1 is that audit; 0.2.2 gives them something
to do.

### philemon

there was a borrowed bootloader here until 0.1.12. writing one and then
booting with somebody else's is not much of a bootloader, so limine is
gone -- and so are the iso, the uefi path, and the boot protocol that
came with it. `make run` boots mine now, because there is nothing else
to boot.

he is named for the one who grants the power and then steps back. he
does not fight anything and he is not there for the rest of it. that is
the whole job description of a bootloader, and it is more than most of
them get.

**he is one program.** the only reason there is a line across the middle
of the file is that the bios reads exactly one sector -- 512 bytes,
ending in `0x55 0xaa` -- drops it at `0x7c00` and jumps to it. that is
the entire contract and it is not negotiable. so the first 512 bytes do
nothing but pull in the rest of the same file, to the address
immediately after themselves, and carry on. after that the line is
invisible: it is one image at one address, and nothing below cares where
the sector ended.

everything the bios can be asked, he asks while it can still answer,
because long mode stops the answering:

- **a20.** the twenty-first address line is still disabled at power on so
  a machine from 1981 could wrap around at one megabyte. it has been
  forty years.
- **unreal mode.** the bios cannot write above one megabyte and the
  kernel does not fit below it. so: into protected mode for exactly long
  enough to load one segment register with a descriptor whose limit is
  the whole address space, and back out again -- returning to real mode
  does not reload the hidden half of a segment register, so the wide
  limit survives and 32-bit offsets keep working. it goes in `fs`, not
  `es`, and that detail is the difference between this working and not:
  coming back to real mode leaves a segment register alone, but *writing*
  to one reloads it with real-mode rules, and every bios call is entitled
  to write to `es`.
- **page tables**, because long mode will not start without them. two
  megabyte pages, three windows: everything identity mapped so the loader
  keeps working, the same memory again in the higher half where the
  kernel expects a direct map, and the kernel's own window at the top.

**the 64-bit half is C**, and that is the point of the split. once long
mode runs there is no reason to stay in assembly, and the work left --
parsing an elf, honouring the distances a linker chose between segments,
zeroing bss, carving everything already spent out of the memory map --
is exacting work where being wrong by eight bytes is a black screen. all
of it compiles for the host and is tested.

**the handoff is one struct.** the kernel is entered the way any function
is called: a pointer in `rdi`. there is no protocol to speak of and
nothing to scan for. everything the kernel could only have learned before
long mode -- the memory map, where it was loaded, the direct map offset,
the ramdisk, the framebuffer, the acpi tables -- is in there.

the assembly cannot be tested on a machine with no qemu, so it says what
it is doing at every step, over the serial port, from the first
instruction of the boot sector onwards. a bootloader that fails silently
is one nobody can fix.

### one namespace, two filesystems

the disk arrived in 0.1.10 bolted on at `/disk`, which had it backwards.
the disk is the big writable persistent thing; the ramdisk is the small
read-only one that is always there. so:

```
/          the disk, when there is one
/boot      the ramdisk, always
```

a name with no leading slash is looked for on the disk first and the
ramdisk second. that one rule is what makes the arrangement useful: a
disk can supply its own `bin/ls` or its own `passwd` and it simply wins,
while a machine with no disk at all falls through to the copies it
booted with and behaves exactly as it did before there was any of this.
`mount` prints the table; `cat welcome.txt` and `cat /boot/welcome.txt`
give different files on purpose, which is the shortest demonstration of
the search order there is.

**the ramdisk is not going anywhere.** it is a module the bootloader
hands over before any driver exists -- no pci, no ahci, no filesystem to
mount. every program and the passwd file live on it, so a kernel whose
only filesystem needed a sata controller would be a kernel that a
missing cable turns into a brick. it is also the obvious foundation for
booting off one medium to install onto another, which is where this
eventually goes.

that claim is worth more than a comment, so the vfs suite runs every
check twice: once with a disk and once with the disk switched off. the
second half asserts that programs still load, passwd is still found, and
the root still honestly reports that it holds nothing but `/boot`.

one deliberate exception to the search order: the boot banner reads
`/boot/welcome.txt` by name rather than searching, so what the machine
says about itself cannot be changed by whatever happens to be sitting on
the data disk.

### a disk, and a filesystem on it

everything before this forgot. the ramdisk is a tar file the loader hands
over at boot -- read-only, in memory, gone when the power goes. this is
the other thing.

**the drive is reached over ahci**, which is what the sata controller
found in 0.1.8 actually speaks. it is nothing like the ide interface it
replaced, where you wrote a command to a port and waited. here the
command is built in ram: a header saying how long it is and where its
table lives, a table holding the frame the drive will receive, and a
scatter list of physical addresses the data should be moved to. then one
bit is set to say slot zero is ready, and the controller does the whole
transfer itself and clears the bit when it is done. the cpu never
touches the bytes.

I poll rather than take an interrupt, and every wait is bounded --
a controller that never answers must not be able to hang the boot, which
is a lesson this project learned the hard way in 0.1.7.

**the filesystem is fat32**, which is worth knowing precisely because it
is so nearly nothing:

- a boot sector describing the layout
- a table with one entry per cluster, where entry N holds the number of
  the cluster that follows N. a linked list with all its pointers
  gathered in one place, which is why it is called a file allocation
  table
- directories, which are not a special kind of object at all -- a
  directory is a file whose contents happen to be 32-byte records

long filenames are the one genuinely strange part. 8.3 names have no
room for `velvet-room.txt`, so the long one is smuggled into extra
records placed *before* the real one, thirteen utf-16 characters at a
time, in three runs at odd offsets because those were the only bytes
left unused. a checksum of the short name ties them together, so a tool
that only understands 8.3 can delete a file without leaving its long
half behind. I read those, and check every piece arrived before
trusting the result -- a name assembled from an incomplete set would be
silently truncated, which is a worse answer than falling back.

writing works: to existing files, past their end (growing the chain a
cluster at a time), and to files that did not exist. new files get short
names only, and a name that will not fit is refused rather than mangled.

tab completion follows the namespace into its directories -- a trailing
slash on directories, so tabbing again carries on into them rather than
stopping at a name that cannot be opened.

**the filesystem never touches hardware.** it is handed two functions
that move sectors, the same shape as the pci scan being handed a way to
read config space. that is what lets the test suite run the real parser
against a real filesystem image on a machine with no disk at all.

which matters, because the formatter and the parser were written by the
same hand -- exactly the trap that has caught this project twice. so the
check that counts is not in the test suite: **mount the image on linux**.
a driver nobody here wrote reading files this kernel wrote is the only
evidence that the layout is right rather than merely self-consistent.

### allocators that stopped searching

the first three allocators here were all honest and all linear. the pmm
walked a bitmap looking for a run of free bits. the heap walked a free
list looking for a block big enough. both get slower the longer the
kernel runs, because a machine that has been up a while has its free
memory in more pieces than a machine that just booted.

**the pmm is a buddy allocator now.** memory is kept as free lists, one
per block size, so taking a block is following a pointer rather than
searching. the interesting half is on the way back: every block has
exactly one partner it could have been split from -- its buddy, found by
flipping a single bit of its frame number -- so freeing means asking "is
my buddy also free?", and if it is, the two merge into one block of the
next size up, and the question is asked again. large blocks reassemble
themselves out of small ones with nobody keeping a record of what was
split from what.

the price is rounding: ask for five pages and you get eight. that waste
is real, so it is counted honestly rather than hidden -- `mem` reports
what is actually consumed. the free lists also give `mem` something it
could not show before, which is the *shape* of free memory:

```
free blocks, by size
  1x4K 1x8K 2x64K 1x512K 511x4M
```

a machine with plenty free and none of it contiguous is a machine about
to fail a large request, and that is invisible in a single "free" number.

**fixed-size things get object caches.** a kernel spends most of its
allocations on a handful of structs whose size never changes -- a
thread, an address space -- and for those, "how big?" and "where does it
fit?" have the same answer every time. a slab cache answers them once:
take a page, cut it into objects of exactly that size, thread a free
list through the unused ones. allocation is taking a list head.

each page carries a small header naming the cache it belongs to, which
is what lets `slab_free` take a bare pointer and work out where it came
from -- mask off the low twelve bits and the answer is right there -- and
what lets a page whose objects have all come home go back to the pmm
instead of being held forever. `slabs` shows the lot:

```
cache            size  /page   live   peak  pages
thread            128     31      4      7      1
addrspace          32    126      0      2      0
kmalloc-64         64     63      9     22      1
```

**and `kmalloc` no longer searches at all.** anything that fits a size
class comes from a slab cache; anything larger takes whole pages with a
sixteen-byte header on the front. which of the two a pointer came from
is written on the page it sits in, so `kfree` costs one read rather than
a walk. the classes are powers of two, so the worst case wastes just
under half -- a good trade in a kernel where nearly every allocation is
one of a dozen structs.

the boot self-test now asks for two contiguous megabytes *after* all its
allocating and freeing is done. if the halves were never merging, that
is where it is found out, rather than the first time something large is
needed.

### what is plugged in

every device on the pci bus answers to 256 bytes of configuration space whose first few fields are identical on all of them: who made it, what it is, and roughly what sort of thing that makes it. reading those is the whole of enumeration, and `lspci` prints the result -- vendor and device ids, a class in words, the base address registers saying where it listens, and its interrupt line.

two parts of a scan are easy to get wrong and are the reason this has a test:

**a multifunction device only admits it in one bit.** a single physical part can present several devices, and the only clue is a bit in function zero's header. miss it and the second half of an ich9 -- its smbus controller, say -- simply does not exist as far as the kernel is concerned.

**a bridge hides a whole bus behind it.** the devices there are every bit as real as the ones in front, and are found only by reading the bridge's secondary bus number and walking through. the walk has a depth limit, because bridges are *supposed* to form a tree and a machine whose firmware disagrees should not be able to make the kernel recurse forever.

the scan takes config space as a function rather than reaching for the ports itself, which is the same trick the memory map and the acpi tables use: the test builds a machine with a bridge, a multifunction part and empty slots between them, and checks the walk finds exactly what is there.

reading config space is two port writes that must not be interleaved with anybody else's, so it is done with interrupts off. there is a memory-mapped way in on newer firmware which reaches further, but nothing here needs the parts it reaches.

### modern interrupt hardware

the 8259 is a single chip the whole machine shares. the local apic is a piece of the processor, which is why every core has one -- and why a kernel meaning to run on more than one cpu cannot be built on the old chip. this version moves everything across: acpi tables to find the hardware, an io apic to route the external interrupts, and the lapic's own timer in place of the pit.

on its own that is **lateral** -- the same interrupts arriving by a better road, and nothing above the interrupt layer can tell. that is worth saying plainly. what it buys is everything after it.

three things had to be got right rather than assumed:

**the numbers everyone knows are wrong.** the pit is wired to irq 0 and the keyboard to irq 1, except that on most real machines they arrive at the io apic on different lines entirely. the madt's *interrupt source override* entries are the only way to know which, and a kernel that skips them gets no timer. so the routing asks acpi rather than asserting, and the polarity and trigger flags are honoured too -- a level-triggered line left as edge fires once and stops; an edge one treated as level fires forever.

**the apics are not in ram.** both sit above every scrap of memory the machine has, so the direct map does not reach them and each needs a page mapped explicitly -- with caching *off*, since these are registers and the cpu must not remember what one of them said last time.

**nobody documents the lapic timer's clock.** it runs at some fraction of the bus speed, which varies by machine, so it has to be measured -- against the pit, which is still ticking and still knows what a second is. calibrate for 50ms, work out the rate, start the periodic timer, and only then tell the pit to stop.

**"is the timer on the lapic" and "are external interrupts on the io apic" are two questions.** they were one flag for about an hour, and the consequence was instructive: with the timer moved and the keyboard left behind, a keypress arriving from the 8259 was acknowledged at the lapic instead. the 8259 never heard that its interrupt had been handled, so it stopped delivering -- one character, then a machine that looked dead. an interrupt has to be acknowledged at whichever chip actually raised it, and while the two halves of this milestone are in different places, only two flags can say which that is.

**and half of it is deliberately not automatic.** the lapic timer can be *proved*: start it, wait by some other means, see whether it delivered, and put the old one back if it did not. an io apic route cannot. its registers read back correctly and it still may deliver nothing -- and the way you find that out is that the keyboard stops, on a machine you can then no longer tell to try something else.

so the boot moves the timer and leaves the keyboard and serial on the 8259, which demonstrably works. moving those too is the `ioapic` command, asked for from a shell that already works, where a mistake costs a reboot rather than the machine. that is a worse default and a far better way to find out. the routing verifies itself by read-back either way; it just cannot verify the part that matters.

**the calibration cannot use the timer it is replacing.** the obvious way to wait 50ms is to count timer interrupts, and that is exactly wrong here: interrupts are still off this early in boot, and the chip that would deliver them is about to be masked. so the wait polls pit channel 2 instead, whose countdown reports itself through a bit on the keyboard controller's port and needs no interrupt at all. i learned this the direct way -- the first version hung on a blank screen.

and then it is **proved before being trusted**. everything up to that point is register writes that either worked or did not, with no way to tell from where you are standing. so interrupts go on briefly, a polled wait passes, and if the new timer delivered nothing the old one goes straight back. ten lines later that would not be possible, and the symptom would be a machine that reaches a prompt and then never sleeps again.

if the firmware describes no apics, none of this happens and the 8259 keeps the job. that is not a failure path bolted on; it is the same behaviour by an older road, and everything above the interrupt layer is written not to care which.

### measuring itself

the timer tick charges itself to whoever was running when it arrived. that is a *sampling* measure rather than real accounting -- a thread that always yielded just before the tick would look free -- and it is worth being clear that is what it is, but it is one line of arithmetic in the interrupt that already existed, and it turns the scheduler from a claim into something you can watch. `ps` grows a cpu column; the idle thread usually holds most of it, which is the honest picture of a machine waiting for somebody to type.

the pmm remembers its high-water mark, because a current figure tells you where you are and a peak tells you how close you came. every syscall is counted as it is dispatched, which is the cheapest possible picture of what a program really does -- afterwards you can say with certainty which doors get used, and `top` lists only the ones that ever were.

`top` redraws all of it twice a second until you press a key. everything it shows already existed somewhere; the point is that a number you watch move tells you something a number you have to ask for twice does not.

### running a program, quietly

`run bin/hello` narrates: which pid it got, where it entered ring 3, when it departed. typing `cat motd.txt` says none of that, because you wanted the file, not a commentary on the reading of it. the difference is a flag on the process, set by the shell depending on how it was asked -- and a kernel thread from `summon` always narrates, since being watched is the entire reason it exists. being *killed* is always reported either way: somebody asked for that and deserves to know it happened.

### users

boot now stops at a login prompt and asks who you are. the accounts come from `passwd` in the ramdisk; the password is not echoed as you type it. every process carries the **uid** of whoever started it, and a program cannot ask to be somebody else -- it runs as the shell's user, and a child inherits its parent's. the prompt shows which you are: `igor@velvet#` for uid 0, `guest@velvet$` for anyone else.

the check that gives this teeth is `open`. tar records a unix mode in every header, and the kernel weighs it against the calling process's uid: uid 0 reads anything, everyone else needs the other-read bit. `velvet-room.txt` ships mode 0600, so `cat velvet-room.txt` works as `igor` and is refused as `guest` -- and `bin/whoami` demonstrates it from ring 3, where it can be told no and do nothing about it.

**the passwords are stored in the clear, and that is not a corner cut to be fixed later.** it is the honest shape of what this demonstrates. the interesting half of a user is not how the password is kept but what the uid can and cannot reach, and that half is enforced by hardware: ring 3, an address space of its own, and a kernel that checks before it hands anything over. without that boundary a "user" is a variable saying you are an admin. storing a password properly needs somewhere to write, which is what a real disk is for.

login says the same thing for a wrong name as for a wrong password, because saying which was wrong hands over half of it.

### the toolbox moved out

`cat`, `echo`, `uptime` and `ls` used to be kernel commands, with the whole kernel in reach. they are programs in `ramdisk/bin` now, running in ring 3 with an address space of their own, able to touch nothing they were not handed. typing `cat motd.txt` looks exactly the same: when a word is not a builtin, the shell looks for a program of that name in `bin/` and runs it.

what made this possible was **arguments**. a program had no way to be told anything, so `cat` could not be told which file. now the loader lays argv out on the program's own stack before it starts -- the strings, then an array of pointers to them -- and hands `argc` and `argv` over in `rdi` and `rsi`. the awkward part is that every store goes through the direct map, while every *pointer written* has to be the address the program will see, since the space it belongs to is not loaded yet.

the point of the exercise was to find out which commands were quietly using kernel internals, and the answer is worth recording:

| | |
|---|---|
| `echo` | needed only argv |
| `uptime` | needed nothing that did not already exist |
| `cat` | needed nothing either -- `open`/`read` arrived in 0.1.2 |
| `ls` | needed one new syscall. `open` can only answer about a name you already know, so listing a directory required `readdir` |

what stayed behind genuinely could not leave: `vmm`, `bt`, `hexdump`, `ps`, `mem`, `dmesg` and `kill` all read kernel state directly, and `crash`, `smash`, `reboot` and `poweroff` exist to do things to the machine rather than with it.

### the keyboard belongs to somebody

before this there was no answer to the question "whose keys are these". the shell sat in a loop peeking at the input while a program ran, watching for ctrl+c and killing on the program's behalf -- which meant a program could never actually read the keyboard, because the shell was standing in front of it.

now the terminal has a **foreground process**. while a program runs it holds the front, and the shell stops watching the keyboard entirely. only the foreground process may read stdin; a background one is refused rather than helping itself to keys meant for whoever is being typed at.

ctrl+c aimed at a program is **delivered to it** rather than acted on for it. it never reaches anybody's input buffer as a character -- it is a request, not a byte. the process finds it on its next syscall: a read comes back -1, and a sleep returns -1 rather than pretending the time passed, so a sleeping program learns about it immediately instead of whenever it next happened to ask for something. what to do about it is then the program's business.

pressing it twice stops being a request. the first is delivered; if the program is still there when a second arrives, the kernel says so and ends it. that is as close to a signal as this kernel gets, and the honest shape of it: a flag the process finds, plus an escape hatch for programs that ignore it.

the terminal also does the **echoing**, which is the part you notice the moment it is missing. a program in ring 3 never sees the keys go past on their way to its buffer, so it cannot echo them itself -- if the terminal does not, you type into a void and see nothing until the program answers. so the tty owns the line discipline: characters appear as they are typed, backspace takes one off the screen as well as out of the buffer, arrows are ignored rather than drawn as nonsense, and the line is handed over on enter. the kernel shell has always done its own version of this for its prompt; now a program gets one too.

`bin/ask` reads a line from the keyboard and greets you, then sleeps in a loop inviting an interrupt -- a program that could not have worked at all one version ago.

### processes outlive their threads

a thread is reaped the instant it dies -- stack and address space handed straight back -- so an exit code kept on the thread would be gone before anyone could read it. the process table is the thing that outlives it: a fixed set of slots holding the pid, the parent, the name and how it ended, staying occupied until somebody collects them. a process that has finished but not been collected is what everyone else calls a zombie, and it is the only reason `run` can tell you a program exited 42.

that also retired the oldest caveat in this file. `kill` used to refuse anything blocked on a waitq, because the queue held a bare pointer to a thread the reaper was about to free. threads now carry a pointer back to the queue they are parked on, so killing one takes it off that queue first -- and the refusal is gone.

### one address space each

every program has its own pml4. only the lower half differs -- the upper half, where the kernel and the direct map live, is shared *by reference*, so all of it stays reachable no matter whose tables are loaded. it has to be shared: the stack I am standing on when I switch cr3 is up there.

sharing by copying the top-level entries means a change the kernel makes later (splitting a huge page for a guard page, say) is seen by every space at once. that only works because the kernel maps everything it will ever need before the first program exists and never adds a new top-level entry afterwards.

it also means teardown is simple and complete: freeing the lower half of a space walks four levels and hands back the program's image, its stack, and the tables that described them. nothing to track by hand, and nothing left behind -- the test for it hands `addrspace.c` a pmm that refuses a double free and checks the books are *exactly* level after two spaces are created, used and destroyed.

`run bin/counter &` twice starts two copies of the same program, at the same entry point, with the same stack address, each writing to a page it believes it owns alone -- and it is true. `ps` shows which threads are ring 3 and how many pages each is holding.

`run` is a foreground command: it waits for the program and gives you the prompt back when it is done, which is what a shell does. `summon` is deliberately the opposite -- it puts a thread in the background and returns at once, because watching threads share a console *is* the demo. while a program runs, ctrl+c stops it; the wait peeks at the key rather than taking it, so a program sitting on `SYS_READ` still gets the input meant for it.

what is missing, kept here where it stays uncomfortable:

- no `fork`. `spawn` will be the shape instead, since fork without copy-on-write is an expensive way to waste memory.
- no demand paging -- every page a program will ever touch is mapped before it starts.
- the scheduler is round-robin with a fixed quantum and no priorities, so a busy thread and an idle one are treated identically.
- the ramdisk is read-only and lives in ram, which is why a real disk is on the roadmap.

## the ramdisk

there is a filesystem, in the sense that a filing cabinet is furniture. `ramdisk/` is tarred up at build time, the loader loads it as a module, and the kernel walks the 512-byte ustar headers to find files. no directories, no writing, and no allocation at all -- `cat` hands you a pointer straight into the archive.

one ordering trap: the module bytes are safe, because the loader types that memory "kernel and modules" and I never reclaim it. the *response structure* describing where they are is in bootloader-reclaimable memory, so `ramdisk_init()` has to copy the address out during early boot, before the shell hands that memory back.

`ls` skips the directory entries GNU tar leaves in the archive, and the parser stops rather than wandering when it meets a header without the ustar magic or a size field that would walk it off the end of the buffer. both of those have tests.

## testing

most of this kernel can be tested without booting anything, because the parts that think are deliberately kept separate from the parts that touch hardware. `pmm_init_from_map()` takes a memory map rather than asking the loader for one, `keyboard_feed()` takes a scancode rather than reading port 0x60, `serial_feed()` takes a byte, `run_line()` takes a string. so the test suites compile the *real* kernel sources as ordinary linux programs and poke at them:

```
$ make test
  checkfmt   ok        every format string vs what kprintf implements
  kprintf    ok        formatting vs the real printf, 33 cases
  mm         ok        pmm + heap, incl. draining ram dry
  vmm        ok        page tables built and walked, 40+ cases
  addrspace  ok        sharing, isolation, and a leak-free teardown
  process    ok        pids, zombies, fds, and a kill that races
  syscall    ok        dispatch, files, spawn, and every pointer refused
  tty        ok        who owns the keyboard, echo, and what ctrl+c means
  auth       ok        accounts, and every malformed line refused
  acpi       ok        firmware tables, and every malformed one refused
  pci        ok        a fabricated machine, walked bridges and all
  buddy      ok        splitting, merging, and every frame handed out once
  slab       ok        object caches, and pages that go back when empty
  fat32      ok        a real image: long names, subdirectories, writes
  vfs        ok        resolution, shadowing, and all of it with no disk
  philemon   ok        the loader's elf parsing and its memory map
  locks      ok        eight threads through the allocators, and a control
  path       ok        `..`, and every way of trying to climb out of /
  args       ok        clustering, values, and `--` meaning what it must
  gdt        ok        the tss descriptor, decoded back apart
  ksyms      ok        symbol lookup, incl. a sweep across boundaries
  rtc        ok        bcd, 12/24 hour, and midnight
  ramdisk    ok        ustar parsing, incl. the real build output
  elf        ok        header validation, incl. the real user program
  keyboard   ok        scancodes, ctrl, arrows, 20 cases
  serial     ok        terminal dialect + escape sequences
  shell      ok        parsing, dispatch, history, 32 cases
  switch     ok        a real context switch, in userspace
  all suites passed
```

the vmm suite is worth a word too: it hands `vmm.c` a malloc'd arena and calls offsets into it "physical addresses", then builds real four-level page tables in it and walks them back -- huge pages, huge-page splitting, misaligned ranges, and running out of frames mid-map. no cpu involved.

the switch one is the interesting one: it runs the actual `switch.asm`, fabricates a stack the same way `thread_create()` does, switches into it, resumes it, and checks all six callee-saved registers came home. that code is miserable to debug inside qemu and trivial to debug when a mistake is just a segfault.

`make test` also runs `tools/checkfmt.py`, which exists because of a bug that cost an afternoon. gcc's `format(printf)` attribute checks my format strings against *real* printf, so it happily accepts any flag the C standard allows -- including ones my little formatter never implemented. a `%-7s` slipped through, got printed literally, and every argument after it was read into the wrong slot; the kernel ended up printing its own machine code as a string and then page faulting a long way from the mistake. the checker compares every `kprintf`/`panic` format string against what `lib/kprintf.c` can actually do, and fails the build otherwise.

host builds define `TINYOS_HOSTED`, which turns `irq_save`/`irq_restore` into no-ops -- userspace gets shot for saying `cli`.

then there is `make boottest`, which builds the iso, boots it headless, and **types commands at the shell over the serial port**, checking the answers. that is only possible because com1 feeds the same input queue the keyboard does. ci runs both on every push.

## notes on memory

the kernel builds its own four-level page tables at boot and moves onto them. the direct map (the loader's hhdm, rebuilt as mine) covers all physical memory with 2MiB pages so the pmm can hand out any frame and I can touch it immediately, and the kernel image is mapped a section at a time with only the rights each one needs:

```
  hhdm    0xffff800000000000 ..  rw-   all of physical memory, 2MiB pages
  the loader  0xffffffff80000000 ..  r--   the request markers
  text    0xffffffff80001000 ..  r-x   executable, not writable
  rodata  0xffffffff80008000 ..  r--   neither
  data    0xffffffff8000c000 ..  rw-   writable, never executable
```

that split is only worth anything with two bits set that are easy to forget: `EFER.NXE`, without which the NX bit is a *reserved bit* and faults on every access rather than doing nothing (so whether I set it is a runtime decision, never a constant), and `CR0.WP`, without which ring 0 may scribble on read-only pages regardless of what the tables say.

switching cr3 is the one operation in this kernel with no diagnostics when it goes wrong -- a bad entry is a triple fault, no message, no register dump, no debugger. so `vmm_init()` walks its own tables in software first and refuses to load cr3 unless the kernel, the direct map, the framebuffer, the page tables themselves and **the stack I am standing on** all resolve to the addresses they should, with the permissions they should. panicking with an explanation beats rebooting in silence.

### giving the loader its memory back

the bootloader's page tables, its structures and its stack all sit in memory it marks reclaimable -- about a megabyte. taking it needs three things to be true first: I must be on my own page tables (done at boot), I must have copied anything I still care about out of the loader's structures, and **nothing may still be standing on the loader's stack**.

that last one is why the shell is its own thread now. `kmain` runs on the stack the loader handed it, so it creates the shell on a pmm-allocated stack and then genuinely exits -- the boot thread dies and the scheduler moves on, and only then is that memory free. the shell reclaims it as its first act. this also meant teaching the reaper that the boot thread is a global rather than a `kmalloc` allocation, so it unlinks it without trying to free it.

the sharp edge is that the loader's *responses* live in that memory too, so every `*_request.response` becomes a dangling pointer the moment the reclaim returns. everything that needs them reads them during early boot, long before.

one thing that is easy to get wrong: the reclaimable regions sit *above* the last usable one on a typical pc, so a bitmap sized only to cover usable ram has no bits for those frames and reclaiming them silently does nothing at all. the bitmap covers both.

### stack overflow, and why the tss exists

a guard page is only half the story. when a thread runs off its stack, `rsp` is already inside the unmapped page by the time the fault happens -- so when the cpu tries to push an exception frame to report the page fault, *that push faults too*. the second failure is a double fault, and with nowhere to push that either, it becomes a triple fault, which on real hardware means the machine simply reboots. no message, no dump.

the fix is the tss, whose slot has been sitting reserved in the gdt since the very first exception handler. its interrupt stack table gives the double fault vector a stack of its own that the cpu switches to unconditionally, healthy or not. cr2 still holds the address of the original fault, so the handler can look at it, notice it lands in the faulting thread's guard page, and say plainly that the thread ran out of stack. try `smash` in the shell.

the tss also carries `rsp0`, the stack the cpu switches to on a ring 3 -> ring 0 trap. nothing uses it yet; when usermode arrives it will have to become per-thread, or two threads trapping at once would land on the same stack.

### guard pages

every thread stack is allocated one page larger than it needs, and that bottom page is then unmapped. a thread that runs off the end of its stack hits the hole and takes a page fault naming itself, instead of quietly chewing through whatever the pmm handed out next -- which, in a kernel with no protection between threads, is some other thread's stack and a bug you would chase for a week. try it with the shell's `smash` command.

punching a 4KiB hole into a 2MiB direct-map page means splitting that page into 512 small ones with identical flags first, which `vmm_unmap_page()` does on demand. the stack has to be handed back the same way round: the guard page gets re-mapped before the frames go back to the pmm, because whoever gets them next will expect to be able to reach them.

the pmm and the heap are both written so their guts can be tested on a normal linux host: `pmm_init_from_map()` takes a memory map + hhdm offset instead of reaching for the loader, so a test can fabricate one over a malloc'd arena. same trick as `keyboard_feed()`. host builds define `TINYOS_HOSTED`, which turns `irq_save()`/`irq_restore()` into no-ops (userspace isnt allowed to `cli`, and has nothing to lock out anyway).

## notes on threads

the context switch saves six callee-saved registers and a return address, and thats the entire parked state of a thread -- everything else the sysv abi already lets a function call clobber. rflags is deliberately *not* saved, because every way of resuming a thread restores it some other way: preempted threads come back through the `iretq` at the end of their interrupt, threads that yielded come back through `irq_restore()`, and brand new threads `sti` for themselves in the bootstrap. that last one is not optional -- a new thread arrives via `ret` with interrupts still off, and forgetting to enable them silently kills preemption for the whole system.

the timer irq sends its EOI *before* running the handler, which looks backwards. its because the scheduler can switch threads inside the timer handler and never return on that stack, and a freshly created thread has no half-finished interrupt frame to return through, so the EOI would never be sent and the pic would go quiet forever.

with preemption live, `kmalloc`/`kfree`/`pmm_alloc`/`pmm_free`/`kprintf` all disable interrupts for their duration. on one core thats the whole locking story: nobody can interrupt me means nobody else can run.

threads that need to wait for something other than the clock park on a `waitq`. the keyboard has one, which is how the shell sits at a prompt costing exactly zero cpu until you press a key. the subtle part is the handoff: `waitq_block()` must be entered with interrupts already off and returns with them still off, so that "look in the buffer, find it empty, go to sleep" is one atomic move. get that wrong and a key arriving in the gap between the check and the sleep is lost forever, and the shell waits for something that already happened.

## what is next

**0.2.0 is more than one cpu**, and the twelve versions of 0.1.x turn
out to have been the easy part. there are 39 `irq_save` pairs in this
kernel and every one of them is a lie on a second core -- turning
interrupts off here says nothing about a thread running there. so the
version after it is an audit rather than a feature, and the one after
that is a scheduler that runs on every core with the tlb shootdown to
go with it.

0.2.x carries on from there: a shell that knows what a directory is,
commands typed by name rather than by path, pipes, an editor, fork and
copy on write, demand paging, a filesystem with opinions about who owns
what, partitions, and an `arch/` boundary that a second architecture is
the only honest way to test. it ends with a live mode that can install
this system onto a disk, which is what the ramdisk has been kept for.

0.3.0 puts it on a wire. see [ROADMAP.md](ROADMAP.md).

## changelog

- **0.2.5** — one argument parser, and a program declares what it takes rather than reading `argv` by hand. that declaration is the only description of the program there is: the parser reads it, and so will whatever has to explain it, which is what keeps usage text from drifting away from the code. short and long forms, clustering, values as the next word or stuck on or after an `=`, and `--` to stop parsing -- the last being the only way to name a file that begins with a dash. `--help` is noticed and deliberately not acted on, because deciding what to print is 0.2.6's job and a parser should not write things. `cat -v` and `cat -n`, `ls -1`, `echo -n`, `write -t`.
- **0.2.4** — a real search path. typing a program by name had worked since 0.1.4, but by sticking `bin/` on the front and asking the ramdisk directly, around the vfs -- so a program on the disk could never be a command and `./thing` meant nothing. now `/bin` then `/boot/bin`, through the vfs, with a name earlier on the path hiding one later; the working directory is deliberately *not* on it, because a name typed alone should mean the same thing wherever you stand and a program left lying about should not become a verb. anything with a slash is a path, read from where you are. `help` is one list with a dot in the margin for the ring 3 ones, and completion offers both. one asymmetry had to go for any of it to work: the ramdisk is a flat archive, so `/boot/bin/hello` could be opened while `/boot/bin` could not be listed.
- **0.2.3** — a working directory per process, and every path resolved against it at the syscall boundary so no filesystem below ever sees a name that means two things. `.`, `..` and repeated slashes are flattened, a path that will not fit is refused rather than truncated, and `..` from the root stays at the root -- that last one having its own tests, since a path that can climb above `/` can name anything. `cd` and `pwd` are builtins because a `cd` that was a program would change where it was standing and then exit. `mkdir` and `rmdir` down to fat32: a directory is born with the two entries every directory has, and only an empty one can be unmade. two gaps surfaced on the way: the mount points could not describe themselves -- `/` is where mounts hang from and `/boot` *is* the ramdisk, so neither is on any filesystem -- and the ramdisk fallback had to widen to absolute paths, or a machine with no disk could suddenly reach nothing at all.
- **0.2.2** — every core runs threads now. one shared ring rather than a queue each, which is a deliberate departure from the roadmap: at four cores the lock is not the bottleneck and an idle core taking whatever is ready is already load balancing, without the migration machinery per-core queues then need. each core gets its own descriptor tables, its own timer and its own idle thread, and learns its own name from the task register -- every core loads a different tss selector, so the register the cpu already holds is its identity, which costs two cycles and needs nothing in memory to have been reached first. tlb shootdown by inter-processor interrupt with a bounded wait for every core to answer. the syscall path turned out to be per-machine where it had to be per core -- the entry stub swapped stacks through two globals, and `star`/`lstar`/`sfmask` were set only on the boot core, so a program scheduled anywhere else executed `syscall` and jumped to address zero in ring 0; it uses per-core words reached through `gs` now -- and deliberately *without* `swapgs`, because the parity of those swaps is per thread while the bases are per core, so a thread preempted inside a syscall and resumed elsewhere leaves a core's bases reversed. three bugs that are not about scheduling had to be fixed first: all four cores were counting the same clock, so an hour would have passed in fifteen minutes; the reaper freed the stacks of threads that another core might still be standing on; and a core's idle thread was visible in the ring before that core had claimed it. `ps` gained a core column, `cpus` says what each is running.
- **0.2.1** — locks, at last. thirty-nine `irq_save` pairs across twelve files were correct mutual exclusion for one core and were quietly reclassified as wrong by 0.2.0; every one is now a real lock. the primitive is deliberately the same shape as what it replaces, so the audit reads as one change repeated thirty-nine times and anything that is not stands out. lock order is checked against a rank read off the call graph rather than assumed -- and it warns rather than panics, because with one core a wrong rank cannot deadlock and should not cost a working machine. `test_locks` is the first suite in this project that can see a race: eight threads through the real allocators, with a deliberately unguarded counter as a control, asserted to come out *wrong* so that the guarded one means something. `locks` shows the ranks and the contention. a lock declared where it is defined never called `spin_init`, so none of the twelve were on the list the shell shows -- they register themselves on first use now. and the recursion check paid for the whole version on its first boot, catching four bugs of one shape: a lock is a property of the machine and must never be held across a context switch, where `cli` was a property of the thread and rode through one harmlessly. a new thread began holding the scheduler's lock with its release on a stack it would never return to; the blocking reader slept holding the keyboard's lock, so the interrupt meant to wake it spun on that lock forever; the timer tick walked the run queue holding nothing; and waking a thread took the scheduler's lock twice. all four were correct on one core.
- **0.2.0** — the other processors wake up. a core that has never run holds itself in reset until another core's local apic tells it otherwise, and when it starts it starts in real mode at a page below a megabyte, so there is a trampoline sitting there to walk it back into long mode -- on page tables that are a copy of the kernel's with low memory identity-mapped, because the kernel maps none of where that code lives. each woken core reports its own apic id, which is the one thing it cannot fake, and then halts. `cpus` lists them. they are given nothing to do on purpose: there are 39 `irq_save` pairs in this kernel that call turning interrupts off mutual exclusion, and every one is false on a second core -- so 0.2.1 is an audit before 0.2.2 is a scheduler. also fixed: `all: $(BOOTIMG)` was written above the line defining `BOOTIMG`, so a bare `make` had been quietly building nothing at all.
- **0.1.12** — philemon, my own bootloader, and now the only one. limine is gone, along with the iso, the uefi path and the protocol that came with it: writing a bootloader and then booting with somebody else's is not much of a bootloader. one file, whose first 512 bytes are the only part the bios will read and which do nothing but pull in the rest of the same file; a20 and unreal mode so the kernel can be read in above a megabyte; page tables and long mode; and a 64-bit half in C that parses the elf and builds the memory map. the kernel is handed one struct in rdi and knows nothing about anybody's boot protocol including mine -- `limine.h` is deleted and there is not one request structure left in it. the C half is host-tested, and writing those tests found two real bugs: a carve loop walking unsorted regions that handed the ramdisk's memory away as free, and boot-table offsets read as though the struct had 16-byte fields. a third was found by reading: the video mode code loaded `fs` in real mode, which quietly undid unreal mode and left the page tables being written somewhere else entirely.
- **0.1.11** — one namespace instead of two filesystems side by side. the disk is the root; the ramdisk moved to `/boot`. a bare name is looked for on the disk first and the ramdisk second, so a disk may supply its own copy of anything while a machine without one falls through to what it booted with. `mount` shows the table, and `cat welcome.txt` versus `cat /boot/welcome.txt` demonstrates the order in one line. the syscall layer lost its prefix tests and its two branches -- `open`, `read`, `write` and `readdir` all go through one resolver now, and programs and `passwd` come through it too, which is what lets either of them live on either filesystem. the ramdisk stays on purpose: it is a module handed over before any driver exists, so a kernel that needed a sata controller to find its own programs would be one a missing cable bricks. the vfs suite runs every check twice, once with the disk switched off, to keep that true.
- **0.1.10** — a disk, and a filesystem on it that remembers. an ahci driver reaches the sata controller pci enumeration found: commands are built in ram -- a header, a table holding the frame the drive receives, a scatter list of physical addresses -- and one bit says go, after which the controller moves every byte itself. polled rather than interrupt-driven, with every wait bounded. on top of that, fat32: cluster chains, subdirectories, and long filenames assembled from the records hidden in front of the short ones (checked for completeness, since a half-assembled name is worse than none). writes go to existing files, past their end, and to files that did not exist yet. `ls`, `cat welcome.txt`, and `write /notes.txt something` -- then reboot and it is still there. the filesystem takes its disk as two functions, so the suite runs the real parser against a real image; but the formatter and the parser share an author, so the check that counts is mounting the image on linux.
- **0.1.9** — the allocators, rebuilt. the pmm is a buddy allocator: free lists per block size, and blocks that put themselves back together when both halves come home, found by flipping one bit of a frame number. fixed-size structs (threads, address spaces) get slab caches, where each page carries a header naming its cache so a bare pointer can be traced back to where it came from -- and so a page whose objects have all returned goes back to the pmm rather than being held forever. `kmalloc` sits on top of those and no longer searches for anything: size classes below 1 KiB, whole pages above. `mem` gained the shape of free memory rather than just the amount of it, and `slabs` shows what each cache is holding. the cost, stated plainly, is that the buddy rounds up -- five pages costs eight -- which `mem` now counts honestly instead of hiding.
- **0.1.8** — the pci bus, enumerated at boot and shown by `lspci`: vendor and device ids, the class in words, the base address registers saying where each device listens, and its interrupt line. the scan takes config space as a function rather than reaching for the ports, so the test builds its own machine -- a bridge with a device behind it, a multifunction part, empty slots between -- and checks the walk finds exactly what is there. the two things worth getting right are that a multifunction part only admits to its other functions in one bit of function zero, and that a bridge hides an entire bus that has to be walked through; the walk is depth-limited, because firmware that disagrees about bridges forming a tree should not be able to make the kernel recurse forever.
- **0.1.7** — off the 8259 and onto the apics. acpi tables walked from the rsdp the loader hands over, the io apic routing external interrupts, and the lapic's own timer in place of the pit -- calibrated against the pit first, because nobody documents what speed it runs at. the routing asks acpi where an irq really arrives rather than assuming the numbers everyone knows, since irq 0 is wired to line 2 on most real machines and a kernel that assumes gets no timer at all. both apics live above ram, so each needs a page mapped with caching off. the calibration polls pit channel 2 rather than counting its interrupts, because interrupts are off that early and the chip is about to be masked -- and the new timer is proved to deliver before the old one is given up. the io apic half is *not* automatic: a route that reads back correctly can still deliver nothing, and the symptom is a machine with no keyboard, so it lives behind an `ioapic` command you run from a shell that already works. and because the timer and the external interrupts now live on different chips, which one acknowledges an interrupt is two flags rather than one -- getting that wrong meant the 8259 never heard back, and stopped after a single keypress. lateral on its own and the readme says so; what it buys is a second cpu being possible. if the firmware describes no apics the 8259 keeps the job and nothing above notices. the parser is tested on malformed tables, because firmware bytes are the least trustworthy in the machine and the ones acted on earliest.
- **0.1.6** — the kernel measures itself. the timer tick charges itself to whoever was running, so `ps` grows a cpu column and the scheduler stops being theoretical -- a sampling measure rather than real accounting, and the README says so. the pmm remembers its peak, every syscall is counted as it is dispatched, and `top` redraws the lot twice a second until you press a key. also quieter: a program typed by name no longer narrates its pid and its departure, because you wanted the output rather than a commentary on it. `run` still does, since that is a demonstration, and a kill is always reported.
- **0.1.5** — users. a login prompt reading accounts from `passwd` in the ramdisk, with the password not echoed; a uid on every process, inherited by children and unaskable-for by programs; and a check with real consequences -- `open` weighs the mode tar recorded against the caller's uid, so `velvet-room.txt` at 0600 is readable by `igor` and refused to `guest`. `whoami` exists twice on purpose: the builtin reads a variable the shell keeps, the program asks the kernel what uid it was given and cannot lie about the answer. the passwords are plaintext and the README says why that is the honest shape of this rather than a corner cut. the parser is tested mostly on malformed input, since a passwd file letting somebody in on a line it half understood is the worst thing it could do.
- **0.1.4** — a toolbox outside the kernel. `cat`, `echo`, `uptime` and `ls` are programs in `ramdisk/bin` now, and typing one looks no different because the shell falls back to looking for a program of that name. what made it possible was arguments: the loader builds argv on the program's own stack -- strings, then pointers to them -- and hands argc and argv over in registers, with every store going through the direct map while every pointer written is the address the program will see. the exercise was meant to reveal which commands were secretly using kernel internals, and it did: `echo` needed only argv, `cat` and `uptime` needed nothing new, and `ls` needed `readdir`, because `open` can only answer about a name you already know. what stayed behind reads kernel state or acts on the machine, and could not have left.
- **0.1.3** — the keyboard belongs to somebody. a foreground process owns the terminal while it runs, and the shell stops peeking at keys on its behalf -- which is what made a program reading the keyboard impossible until now. ctrl+c aimed at a program is delivered to it rather than acted on for it: it never becomes a character, the process finds it on its next syscall, and a read or a sleep comes back -1 so a sleeping program hears about it at once. pressing it twice stops asking. only the foreground process may read stdin, so a background one cannot take keys meant for somebody else. the tty also owns the line discipline -- echo, backspace, and ignoring arrows -- because a program never sees the keys go past and cannot echo them itself; without it you type into a void. `bin/ask` reads a line and greets you, which is a thing that could not have worked a version ago.
- **0.1.2** — the syscall table doubles: `open`/`close` and a `read` that takes a descriptor, so a program can read a file instead of only being loaded from one; `getpid`; and `spawn`/`wait`, which let a program start another and hear how it went. descriptors live on the process, so they close when it does, and a bookmark into a read-only archive costs nothing to allocate or free. a process may only wait for its own children. `read`/`write` gained an fd argument, a breaking change to the user abi and the right shape. also fixes a regression 0.1.0 shipped: user pointers were validated against the kernel's page tables, which since per-process address spaces map none of a program's memory -- so every syscall taking a pointer silently returned -1 and programs printed nothing at all. refusals are logged now, a test asserts which page tables get consulted, and the boot test fails if any pointer is ever refused. two new programs: `bin/reader` opens a file and reads it in bites, `bin/parent` spawns `bin/fail` and passes on its 42.
- **0.1.1** — processes. a program now has a pid, a parent and an exit code, kept in a table that outlives the thread that ran it -- which is the only way an exit code can survive, since the thread and its whole address space are gone the moment it dies. `run` reports how a program went; `ps` shows threads and processes as the different things they are. and the caveat that has been in this file since m6 is retired: threads carry a pointer back to the waitq they are parked on, so `kill` can take one off that queue before the reaper frees it, instead of refusing. `bin/fail` exists to exit 42 and prove the number gets home.
- **0.1.0** — **programs are isolated.** each gets its own pml4, sharing only the kernel half, and by reference so the kernel stays reachable whichever tables are loaded -- it must, since the stack I switch on lives there. two copies of the same program now run at once at identical addresses without meeting. teardown walks the lower half and hands back the image, the stack and the page tables together, which retires the leak 0.0.17 shipped with. `run prog &` for background, `ps` showing which threads are ring 3 and how much memory each holds, and `bin/counter` as a second program that exists to be run twice. plus [ROADMAP.md](ROADMAP.md), which lays out the eleven steps of 0.1.x -- from exit codes and a wider syscall table up to a filesystem on a real disk and a bootloader of my own.
- **0.0.17** — **it runs programs.** ring 3 via `iretq` into a fabricated frame, `syscall`/`sysret` with STAR/LSTAR/SFMASK, a static elf64 loader, per-thread kernel stacks tracked in the tss and for `syscall`, and `user/hello.c` -- a real program with no libc that prints and sleeps and exits, all through six syscalls. every pointer ring 3 hands the kernel is checked against the page tables before it is touched, mapped *and* user, so a program cannot make the kernel fault by lying -- and the refusals are tested harder than the successes, since they are the actual boundary. found a genuine bug on the way: intermediate page table entries never set `PTE_USER`, and since the cpu ANDs that bit down the whole chain, every user mapping would have been unreachable while looking perfectly correct in a dump. boot is quiet now -- the driver chatter goes to serial and `dmesg`, and the screen gets the banner and `welcome.txt`. tab completes filenames after any command that takes one (`cat`, `run`), fills in the longest shared prefix, and does nothing on an empty word *in the command position* -- listing every command is what `help` is for, but after `cat ` there is no such list to consult, so an empty word there is worth answering. `run` waits for its program like a foreground command should, with ctrl+c to stop it. also `cat` takes several files, unknown commands suggest the nearest match (and a bare filename points at the path it lives under), `ls` prints paths you can actually retype, and `ps` prints in id order.
- **0.0.16** — files. `ramdisk/` becomes a ustar tar at build time, the loader passes it as a module, and `ls`/`cat` read straight out of it with no copying. the parser is fed hand-built archives in the tests -- block-sized files, empty files, gnu tar's leading `./`, a header with no magic, a size field that lies -- and then the real archive the build produces, which is the one that catches what tar actually emits. also ctrl+l to clear without losing the line, and two commands that had to be persona-inspired: `arcana` for the version, rendered as the rank of a social link, and `persona`, a fastfetch that shows the machine's face along with what cpu it wears.
- **0.0.15** — a real line editor. cursor movement and mid-line editing, ctrl+a/e/w/u/k, del, and tab completion. the enabling change was making the console's `\b` non-destructive like an actual terminal, which meant giving it a shadow buffer of the text on screen so the block cursor can sit on a character and put it back afterwards. new commands: `poweroff` (so you stop killing qemu), `date` off the cmos clock, `hexdump` that checks the page tables before reading, `kill`, `history` and `time`. the rtc's decoding is split from its io and tested -- bcd, the pm bit hiding in the top of the hour byte, and 12am being hour zero are each their own small trap.
- **0.0.14** — symbolized backtraces. `tools/gensyms.py` turns the kernel's own `nm` output into a table baked into a `.ksyms` section, and panics, exception dumps and double faults all print a symbolized call chain. the section sits after `.text` so folding it in can never move a function, and the build verifies that rather than trusting it. found two things on the way: my `backtrace()` was colliding with glibc's in the host tests and being silently shadowed (now `kbacktrace`), and the test binaries had no prerequisite on the kernel sources they `#include`, so they were happily running against stale builds.
- **0.0.13** — a tss at last, with an IST stack for the double fault vector. that turns stack overflow from a silent triple-fault reboot into a report naming the thread and its guard page, because the cpu can always find a good stack for that vector even when `rsp` is in the hole. and the pmm now reclaims the loader's memory (~1 MiB): the shell moved onto its own pmm-backed thread so the boot thread can exit and stop standing on the loader's stack, and the bitmap grew to cover the reclaimable regions, which sit above the last usable one and were previously off the end of the map entirely. new test suite for the tss descriptor encoding, which scatters a base address across two qwords and fails silently when you get it wrong.
- **0.0.12** — kprintf learned the `-` (left justify) flag, which it had been claiming to support by virtue of gcc's format checking without ever implementing. the vmm's boot log used `%-7s`, so the specifier printed literally, every following argument landed in the wrong slot, and the kernel read `__data_end` as a string and page faulted. added `tools/checkfmt.py` to `make test` so no format string can outrun the formatter again.
- **0.0.11** — my own page tables. four levels built at boot, direct map in 2MiB pages, kernel mapped per-section with W^X, NX enabled properly via EFER (and treated as a runtime capability, since a hardcoded NX bit faults on a cpu that lacks it), CR0.WP set so read-only means read-only even in ring 0. `vmm_init` verifies the whole thing by walking its own tables in software -- including the current stack -- before daring to load cr3. guard pages under every thread stack, which needed 2MiB page splitting to punch a hole in the direct map. exception dumps now name the thread that died and say when the address is a guard page. new shell commands: `vmm` to look up any address, `smash` to run off the end of the stack on purpose. 40-odd host assertions for the page table code, because a mistake there is a triple fault with nothing to read.
- **0.0.10** — milestone 7. serial input on irq4, with a translation layer for the terminal dialect (cr means enter, del means backspace, `ESC[A` means up) so the shell is drivable over the wire. keyboard and serial now feed one shared input queue in `drivers/input.c` instead of the keyboard owning the buffer privately. six host test suites moved into `tests/` behind `make test`, plus `tools/boottest.sh` which boots the iso and types at it. github actions runs the lot on every push. panics can now be escaped over serial too, not just from the keyboard.
- **0.0.9** — the shell grew the things you immediately miss when you sit down at it. the keyboard driver now decodes ctrl as a modifier (ctrl+letter arrives as a control code, so ctrl+c is 3) and stops throwing away the e0-prefixed arrow keys, which meant widening the ring buffer to 16 bits so arrows cant be mistaken for characters. on top of that: 16 lines of command history on up/down, ctrl+c to abandon a line and recall running personas, and a panic you can escape -- it polls the 8042 by hand and resets on any keypress instead of halting forever and making you kill qemu. keyboard and shell tests grew to 20 and 32 cases.
- **0.0.8** — milestone 6. an interactive shell with line editing and nine commands, running as a real thread (the boot thread renames itself `shell` and takes the job). a waitq in the scheduler plus a blocking `keyboard_getchar_blocking()`, so the prompt costs nothing while it waits instead of spinning on hlt. `summon` spawns persona threads on demand, which replaces the m5 demo threads that used to print forever and made the console unusable. the `FAULT_DEMO` build flag is gone -- the `crash` command does the same job better, and from thread context rather than early boot. shell parsing and dispatch are host-tested (20 cases, incl. argv clamping and empty lines).
- **0.0.7** — milestone 5. the pit ticks at 100hz on irq0 with a global tick counter and uptime. threads: kernel stacks from the pmm, a fabricated initial stack so a brand new thread can be "resumed" into existence, and a 16-instruction context switch in asm. preemptive round-robin scheduling on a 50ms quantum, blocking `sleep_ms()`, `thread_exit()` with a reaper that frees dead threads' stacks (from a different thread's stack, which is the only safe way). an idle thread that hlts so theres always somebody to hand the cpu to. the allocators and kprintf take interrupts down while they work, since a half-updated free list is nobodys friend. demo: pixie and jack-frost count at different rates, the herald says its piece and dies to give the reaper something to do, and typing still works throughout. the context switch is host-tested, including whether all six callee-saved registers actually survive a round trip.
- **0.0.6** — milestone 4. physical memory manager: parses the loader's memory map (and prints it at boot), bitmap over every 4k frame, contiguous multi-page allocation with a rotating search hint, stats. kernel heap on top: first-fit free list, 16-byte aligned payloads, magic-guarded headers that catch double frees and wild pointers, address-ordered coalescing, grows by whole pages from the pmm. boot runs a self-test over both (8 frames + 5 heap blocks, pattern verified, freed out of order, books must balance) and panics if anything is off. tested on the host too, 25 assertions incl. draining ram dry and checking no frame is ever handed out twice.
- **0.0.5** — milestone 3. 8259 pic remapped to vectors 32-47 with spurious irq filtering, an irq_register() layer so drivers can claim lines, and a ps/2 keyboard driver: scancode set 1 -> ascii, shift + capslock state (they cancel, as the gods intended), e0 prefixes swallowed, all landing in a ring buffer. the scancode state machine is split from the irq handler and tested on the host (11 scenarios). boot now ends at a prompt that echoes thy keystrokes, live.
- **0.0.4** — milestone 2. my own gdt (tss slot reserved), idt with 256 macro-generated isr stubs, and an exception handler that prints the vector name, decoded page fault info (cr2 + error bits) and a full register dump before panicking. at the time this shipped with a `FAULT_DEMO=1` build flag to trigger it; as of 0.0.8 thats the shell's `crash` command instead. the kernel also now boots, panics and (eventually) reboots with the appropriate persona social link ceremony. thou art I, and I am thou.
- **0.0.3** — milestone 1 done. kprintf (with actual tested number formatting), framebuffer console with the spleen 8x16 font, glyph blitting, scrolling, block cursor, and panic(). boot banner shows up on screen and serial at the same time. the temporary decimal-printer hack from 0.0.2 is gone, unmourned.
- **0.0.2** — com1 uart driver (polled, 115200 8n1, with loopback self test). framebuffer request to the loader, boot info logged over serial, test pattern on screen. run `make run` and watch the serial chatter in your terminal.
- **0.0.1** — project scaffold. the loader v9.x boots a stub kernel that halts politely. build system, linker script, license, this readme.
