# tinyOS

![ci](https://github.com/USERNAME/tinyOS/actions/workflows/ci.yml/badge.svg)

a tiny 64-bit hobby kernel for x86_64, written in C, booted with [Limine](https://github.com/limine-bootloader/limine).

im building this to actually understand what happens between "power button" and "shell prompt". its not trying to be the next linux, its trying to fit in my head.

**version: 0.1.7** (**off the 8259 and onto the apics.** acpi tables read, interrupts routed through an io apic, and the timer moved to the local apic -- lateral on its own, and the thing a second cpu would need)

## what it does

- [x] boot into 64-bit long mode via limine
- [x] serial (com1) logging, and serial input too -- the shell answers either way
- [x] framebuffer console with its own font rendering
- [x] gdt/idt, real exception dumps instead of silent triple faults
- [x] ps/2 keyboard driver (interrupt driven, no polling)
- [x] physical page allocator + kmalloc heap on top
- [x] pit timer + preemptive round-robin scheduler with kernel threads
- [x] an interactive shell with line editing, history and tab completion
- [x] our own page tables: W^X, NX, guard pages under thread stacks
- [x] a tss with an IST, so a stack overflow reports instead of rebooting
- [x] symbolized backtraces on panic
- [x] a read-only ramdisk, unpacked from a tar limine hands us at boot
- [x] ring 3, `syscall`/`sysret`, and an elf loader -- it runs programs
- [x] an address space per program, reclaimed when it dies
- [x] a process table: pids, parents, and exit codes that outlive the thread
- [x] file descriptors, and `spawn`/`wait` -- a program can start a program
- [x] a controlling terminal: a foreground process, and ctrl+c delivered to it
- [x] arguments, and a toolbox that lives outside the kernel
- [x] users: a login, a uid per process, and files a guest may not read
- [x] cpu accounting, peak memory, syscall counts, and a live `top`
- [x] acpi, the lapic and io apic, and a timer that is part of the cpu
- [x] ci that builds the iso, boot-tests it in qemu, and types at the shell

where it goes next is [ROADMAP.md](ROADMAP.md): eleven more steps, ending in a
filesystem on a real disk and a bootloader of our own.

still a non-goal: networking, and being useful in any practical sense.

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
limine.conf           bootloader config
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

cancelling them with ctrl+c is cooperative, not forceful -- we have no signals and no safe way to yank a sleeping thread off the run queue, so a persona notices it has been recalled the next time it wakes up. that can be up to one sleep period later.

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

there is an obvious chicken and egg here: the table records addresses, and linking the table in changes addresses. the way out is placement. `.ksyms` sits *after* `.text` in the linker script, so folding it in shifts `.data` around but cannot move a single function. that makes a plain two-pass build correct rather than something we have to iterate to a fixed point. and because "cannot" is doing a lot of work in that sentence, the build runs `gensyms.py --check` afterwards, which re-reads the finished binary and fails loudly if any symbol the table describes has moved.

the walker itself assumes frame pointers, so the kernel builds with `-fno-omit-frame-pointer`. it is normally called from a panic, which means nothing in it may fault -- a page fault inside the backtrace printer would bury the real bug under a second one. so every frame pointer is checked against the page tables with `vmm_translate` before being dereferenced, and the walk stops the moment the chain stops making sense (a caller's frame must be at a higher address, stacks growing down as they do).

`bt` in the shell prints a trace with nothing on fire, which is a good way to see it work.

## ring 3

`run bin/hello` loads an elf out of the ramdisk and gives it the outer ring. it cannot touch a port, cannot read the kernel, and cannot see any memory but its own -- the only thing it can do to the world is ask, through `syscall`, and be answered.

`ls` prints paths exactly as `cat` and `run` accept them -- tar stores `./bin/hello`, and showing that verbatim tells you to type something that then does not work. there is no path search, so `run hello` will not find `bin/hello`; instead of adding magic that surprises you later, a miss says which file you probably meant.

`user/` is a whole tiny userland: a freestanding program with no libc, six inline syscall stubs, and its own linker script putting it at `0x400000` in the low half where the kernel's mappings can never reach.

three things about this were easy to get wrong and interesting to get right:

**two calling conventions meet at the entry stub, and they are not the same one.** ring 3 hands over `nr` in rax and arguments in rdi/rsi/rdx/r10/r8; sysv C wants them in rdi/rsi/rdx/rcx/r8/r9, and the dispatcher's *first* argument is the number, so everything shifts one place right to make room. get that wrong and the kernel reads an argument as the call number, which looks exactly like a program asking for syscall 4198426. the shuffle also has to be written in an order where every register is read before anything overwrites it.

**ring 3 gets every register back but three.** the syscall instruction destroys rcx and r11, and rax carries the result -- everything else the user compiled against the assumption that it survives. so the entry stub has to preserve the caller-saved registers itself, because the C dispatcher is free to clobber them and the argument shuffle certainly does. miss that and kernel values leak into a program that will use them as pointers, which surfaces as a page fault in userspace at an address that means nothing to anyone. (rbx, rbp and r12-r15 need no saving there: `syscall_dispatch` is an ordinary C function and the abi makes those its problem.)

**`syscall` does not switch stacks.** it puts the return address in rcx, the flags in r11, loads cs and rip from MSRs, and that is all. you arrive in ring 0 *standing on the user's stack*, which is as alarming as it sounds. the entry stub's first job is to get off it. it can do that with a global only because `SFMASK` clears IF, so we arrive with interrupts off and nothing can preempt us in the three instructions before the user's rsp is safely parked on a kernel stack.

**the gdt layout is not ours to choose.** `sysret` computes `CS = STAR[63:48] + 16` and `SS = STAR[63:48] + 8`, so user data has to sit eight bytes below user code or returning to ring 3 lands nowhere. there is a test asserting that relationship, because it is the sort of thing a tidy-up would quietly break.

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

every program has its own pml4. only the lower half differs -- the upper half, where the kernel and the direct map live, is shared *by reference*, so all of it stays reachable no matter whose tables are loaded. it has to be shared: the stack we are standing on when we switch cr3 is up there.

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

there is a filesystem, in the sense that a filing cabinet is furniture. `ramdisk/` is tarred up at build time, limine loads it as a module, and the kernel walks the 512-byte ustar headers to find files. no directories, no writing, and no allocation at all -- `cat` hands you a pointer straight into the archive.

one ordering trap: the module bytes are safe, because limine types that memory "kernel and modules" and we never reclaim it. the *response structure* describing where they are is in bootloader-reclaimable memory, so `ramdisk_init()` has to copy the address out during early boot, before the shell hands that memory back.

`ls` skips the directory entries GNU tar leaves in the archive, and the parser stops rather than wandering when it meets a header without the ustar magic or a size field that would walk it off the end of the buffer. both of those have tests.

## testing

most of this kernel can be tested without booting anything, because the parts that think are deliberately kept separate from the parts that touch hardware. `pmm_init_from_map()` takes a memory map rather than asking limine for one, `keyboard_feed()` takes a scancode rather than reading port 0x60, `serial_feed()` takes a byte, `run_line()` takes a string. so the test suites compile the *real* kernel sources as ordinary linux programs and poke at them:

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

`make test` also runs `tools/checkfmt.py`, which exists because of a bug that cost an afternoon. gcc's `format(printf)` attribute checks our format strings against *real* printf, so it happily accepts any flag the C standard allows -- including ones our little formatter never implemented. a `%-7s` slipped through, got printed literally, and every argument after it was read into the wrong slot; the kernel ended up printing its own machine code as a string and then page faulting a long way from the mistake. the checker compares every `kprintf`/`panic` format string against what `lib/kprintf.c` can actually do, and fails the build otherwise.

host builds define `TINYOS_HOSTED`, which turns `irq_save`/`irq_restore` into no-ops -- userspace gets shot for saying `cli`.

then there is `make boottest`, which builds the iso, boots it headless, and **types commands at the shell over the serial port**, checking the answers. that is only possible because com1 feeds the same input queue the keyboard does. ci runs both on every push.

## notes on memory

the kernel builds its own four-level page tables at boot and moves onto them. the direct map (limine's hhdm, rebuilt as ours) covers all physical memory with 2MiB pages so the pmm can hand out any frame and we can touch it immediately, and the kernel image is mapped a section at a time with only the rights each one needs:

```
  hhdm    0xffff800000000000 ..  rw-   all of physical memory, 2MiB pages
  limine  0xffffffff80000000 ..  r--   the request markers
  text    0xffffffff80001000 ..  r-x   executable, not writable
  rodata  0xffffffff80008000 ..  r--   neither
  data    0xffffffff8000c000 ..  rw-   writable, never executable
```

that split is only worth anything with two bits set that are easy to forget: `EFER.NXE`, without which the NX bit is a *reserved bit* and faults on every access rather than doing nothing (so whether we set it is a runtime decision, never a constant), and `CR0.WP`, without which ring 0 may scribble on read-only pages regardless of what the tables say.

switching cr3 is the one operation in this kernel with no diagnostics when it goes wrong -- a bad entry is a triple fault, no message, no register dump, no debugger. so `vmm_init()` walks its own tables in software first and refuses to load cr3 unless the kernel, the direct map, the framebuffer, the page tables themselves and **the stack we are standing on** all resolve to the addresses they should, with the permissions they should. panicking with an explanation beats rebooting in silence.

### giving limine its memory back

the bootloader's page tables, its structures and its stack all sit in memory it marks reclaimable -- about a megabyte. taking it needs three things to be true first: we must be on our own page tables (done at boot), we must have copied anything we still care about out of limine's structures, and **nothing may still be standing on limine's stack**.

that last one is why the shell is its own thread now. `kmain` runs on the stack limine handed it, so it creates the shell on a pmm-allocated stack and then genuinely exits -- the boot thread dies and the scheduler moves on, and only then is that memory free. the shell reclaims it as its first act. this also meant teaching the reaper that the boot thread is a global rather than a `kmalloc` allocation, so it unlinks it without trying to free it.

the sharp edge is that limine's *responses* live in that memory too, so every `*_request.response` becomes a dangling pointer the moment the reclaim returns. everything that needs them reads them during early boot, long before.

one thing that is easy to get wrong: the reclaimable regions sit *above* the last usable one on a typical pc, so a bitmap sized only to cover usable ram has no bits for those frames and reclaiming them silently does nothing at all. the bitmap covers both.

### stack overflow, and why the tss exists

a guard page is only half the story. when a thread runs off its stack, `rsp` is already inside the unmapped page by the time the fault happens -- so when the cpu tries to push an exception frame to report the page fault, *that push faults too*. the second failure is a double fault, and with nowhere to push that either, it becomes a triple fault, which on real hardware means the machine simply reboots. no message, no dump.

the fix is the tss, whose slot has been sitting reserved in the gdt since the very first exception handler. its interrupt stack table gives the double fault vector a stack of its own that the cpu switches to unconditionally, healthy or not. cr2 still holds the address of the original fault, so the handler can look at it, notice it lands in the faulting thread's guard page, and say plainly that the thread ran out of stack. try `smash` in the shell.

the tss also carries `rsp0`, the stack the cpu switches to on a ring 3 -> ring 0 trap. nothing uses it yet; when usermode arrives it will have to become per-thread, or two threads trapping at once would land on the same stack.

### guard pages

every thread stack is allocated one page larger than it needs, and that bottom page is then unmapped. a thread that runs off the end of its stack hits the hole and takes a page fault naming itself, instead of quietly chewing through whatever the pmm handed out next -- which, in a kernel with no protection between threads, is some other thread's stack and a bug you would chase for a week. try it with the shell's `smash` command.

punching a 4KiB hole into a 2MiB direct-map page means splitting that page into 512 small ones with identical flags first, which `vmm_unmap_page()` does on demand. the stack has to be handed back the same way round: the guard page gets re-mapped before the frames go back to the pmm, because whoever gets them next will expect to be able to reach them.

the pmm and the heap are both written so their guts can be tested on a normal linux host: `pmm_init_from_map()` takes a memory map + hhdm offset instead of reaching for limine, so a test can fabricate one over a malloc'd arena. same trick as `keyboard_feed()`. host builds define `TINYOS_HOSTED`, which turns `irq_save()`/`irq_restore()` into no-ops (userspace isnt allowed to `cli`, and has nothing to lock out anyway).

## notes on threads

the context switch saves six callee-saved registers and a return address, and thats the entire parked state of a thread -- everything else the sysv abi already lets a function call clobber. rflags is deliberately *not* saved, because every way of resuming a thread restores it some other way: preempted threads come back through the `iretq` at the end of their interrupt, threads that yielded come back through `irq_restore()`, and brand new threads `sti` for themselves in the bootstrap. that last one is not optional -- a new thread arrives via `ret` with interrupts still off, and forgetting to enable them silently kills preemption for the whole system.

the timer irq sends its EOI *before* running the handler, which looks backwards. its because the scheduler can switch threads inside the timer handler and never return on that stack, and a freshly created thread has no half-finished interrupt frame to return through, so the EOI would never be sent and the pic would go quiet forever.

with preemption live, `kmalloc`/`kfree`/`pmm_alloc`/`pmm_free`/`kprintf` all disable interrupts for their duration. on one core thats the whole locking story: nobody can interrupt me means nobody else can run.

threads that need to wait for something other than the clock park on a `waitq`. the keyboard has one, which is how the shell sits at a prompt costing exactly zero cpu until you press a key. the subtle part is the handoff: `waitq_block()` must be entered with interrupts already off and returns with them still off, so that "look in the buffer, find it empty, go to sleep" is one atomic move. get that wrong and a key arriving in the gap between the check and the sleep is lost forever, and the shell waits for something that already happened.

## changelog

- **0.1.7** — off the 8259 and onto the apics. acpi tables walked from the rsdp limine hands over, the io apic routing external interrupts, and the lapic's own timer in place of the pit -- calibrated against the pit first, because nobody documents what speed it runs at. the routing asks acpi where an irq really arrives rather than assuming the numbers everyone knows, since irq 0 is wired to line 2 on most real machines and a kernel that assumes gets no timer at all. both apics live above ram, so each needs a page mapped with caching off. the calibration polls pit channel 2 rather than counting its interrupts, because interrupts are off that early and the chip is about to be masked -- and the new timer is proved to deliver before the old one is given up. the io apic half is *not* automatic: a route that reads back correctly can still deliver nothing, and the symptom is a machine with no keyboard, so it lives behind an `ioapic` command you run from a shell that already works. and because the timer and the external interrupts now live on different chips, which one acknowledges an interrupt is two flags rather than one -- getting that wrong meant the 8259 never heard back, and stopped after a single keypress. lateral on its own and the readme says so; what it buys is a second cpu being possible. if the firmware describes no apics the 8259 keeps the job and nothing above notices. the parser is tested on malformed tables, because firmware bytes are the least trustworthy in the machine and the ones acted on earliest.
- **0.1.6** — the kernel measures itself. the timer tick charges itself to whoever was running, so `ps` grows a cpu column and the scheduler stops being theoretical -- a sampling measure rather than real accounting, and the README says so. the pmm remembers its peak, every syscall is counted as it is dispatched, and `top` redraws the lot twice a second until you press a key. also quieter: a program typed by name no longer narrates its pid and its departure, because you wanted the output rather than a commentary on it. `run` still does, since that is a demonstration, and a kill is always reported.
- **0.1.5** — users. a login prompt reading accounts from `passwd` in the ramdisk, with the password not echoed; a uid on every process, inherited by children and unaskable-for by programs; and a check with real consequences -- `open` weighs the mode tar recorded against the caller's uid, so `velvet-room.txt` at 0600 is readable by `igor` and refused to `guest`. `whoami` exists twice on purpose: the builtin reads a variable the shell keeps, the program asks the kernel what uid it was given and cannot lie about the answer. the passwords are plaintext and the README says why that is the honest shape of this rather than a corner cut. the parser is tested mostly on malformed input, since a passwd file letting somebody in on a line it half understood is the worst thing it could do.
- **0.1.4** — a toolbox outside the kernel. `cat`, `echo`, `uptime` and `ls` are programs in `ramdisk/bin` now, and typing one looks no different because the shell falls back to looking for a program of that name. what made it possible was arguments: the loader builds argv on the program's own stack -- strings, then pointers to them -- and hands argc and argv over in registers, with every store going through the direct map while every pointer written is the address the program will see. the exercise was meant to reveal which commands were secretly using kernel internals, and it did: `echo` needed only argv, `cat` and `uptime` needed nothing new, and `ls` needed `readdir`, because `open` can only answer about a name you already know. what stayed behind reads kernel state or acts on the machine, and could not have left.
- **0.1.3** — the keyboard belongs to somebody. a foreground process owns the terminal while it runs, and the shell stops peeking at keys on its behalf -- which is what made a program reading the keyboard impossible until now. ctrl+c aimed at a program is delivered to it rather than acted on for it: it never becomes a character, the process finds it on its next syscall, and a read or a sleep comes back -1 so a sleeping program hears about it at once. pressing it twice stops asking. only the foreground process may read stdin, so a background one cannot take keys meant for somebody else. the tty also owns the line discipline -- echo, backspace, and ignoring arrows -- because a program never sees the keys go past and cannot echo them itself; without it you type into a void. `bin/ask` reads a line and greets you, which is a thing that could not have worked a version ago.
- **0.1.2** — the syscall table doubles: `open`/`close` and a `read` that takes a descriptor, so a program can read a file instead of only being loaded from one; `getpid`; and `spawn`/`wait`, which let a program start another and hear how it went. descriptors live on the process, so they close when it does, and a bookmark into a read-only archive costs nothing to allocate or free. a process may only wait for its own children. `read`/`write` gained an fd argument, a breaking change to the user abi and the right shape. also fixes a regression 0.1.0 shipped: user pointers were validated against the kernel's page tables, which since per-process address spaces map none of a program's memory -- so every syscall taking a pointer silently returned -1 and programs printed nothing at all. refusals are logged now, a test asserts which page tables get consulted, and the boot test fails if any pointer is ever refused. two new programs: `bin/reader` opens a file and reads it in bites, `bin/parent` spawns `bin/fail` and passes on its 42.
- **0.1.1** — processes. a program now has a pid, a parent and an exit code, kept in a table that outlives the thread that ran it -- which is the only way an exit code can survive, since the thread and its whole address space are gone the moment it dies. `run` reports how a program went; `ps` shows threads and processes as the different things they are. and the caveat that has been in this file since m6 is retired: threads carry a pointer back to the waitq they are parked on, so `kill` can take one off that queue before the reaper frees it, instead of refusing. `bin/fail` exists to exit 42 and prove the number gets home.
- **0.1.0** — **programs are isolated.** each gets its own pml4, sharing only the kernel half, and by reference so the kernel stays reachable whichever tables are loaded -- it must, since the stack we switch on lives there. two copies of the same program now run at once at identical addresses without meeting. teardown walks the lower half and hands back the image, the stack and the page tables together, which retires the leak 0.0.17 shipped with. `run prog &` for background, `ps` showing which threads are ring 3 and how much memory each holds, and `bin/counter` as a second program that exists to be run twice. plus [ROADMAP.md](ROADMAP.md), which lays out the eleven steps of 0.1.x -- from exit codes and a wider syscall table up to a filesystem on a real disk and a bootloader of our own.
- **0.0.17** — **it runs programs.** ring 3 via `iretq` into a fabricated frame, `syscall`/`sysret` with STAR/LSTAR/SFMASK, a static elf64 loader, per-thread kernel stacks tracked in the tss and for `syscall`, and `user/hello.c` -- a real program with no libc that prints and sleeps and exits, all through six syscalls. every pointer ring 3 hands the kernel is checked against the page tables before it is touched, mapped *and* user, so a program cannot make the kernel fault by lying -- and the refusals are tested harder than the successes, since they are the actual boundary. found a genuine bug on the way: intermediate page table entries never set `PTE_USER`, and since the cpu ANDs that bit down the whole chain, every user mapping would have been unreachable while looking perfectly correct in a dump. boot is quiet now -- the driver chatter goes to serial and `dmesg`, and the screen gets the banner and `welcome.txt`. tab completes filenames after any command that takes one (`cat`, `run`), fills in the longest shared prefix, and does nothing on an empty word *in the command position* -- listing every command is what `help` is for, but after `cat ` there is no such list to consult, so an empty word there is worth answering. `run` waits for its program like a foreground command should, with ctrl+c to stop it. also `cat` takes several files, unknown commands suggest the nearest match (and a bare filename points at the path it lives under), `ls` prints paths you can actually retype, and `ps` prints in id order.
- **0.0.16** — files. `ramdisk/` becomes a ustar tar at build time, limine passes it as a module, and `ls`/`cat` read straight out of it with no copying. the parser is fed hand-built archives in the tests -- block-sized files, empty files, gnu tar's leading `./`, a header with no magic, a size field that lies -- and then the real archive the build produces, which is the one that catches what tar actually emits. also ctrl+l to clear without losing the line, and two commands that had to be persona-inspired: `arcana` for the version, rendered as the rank of a social link, and `persona`, a fastfetch that shows the machine's face along with what cpu it wears.
- **0.0.15** — a real line editor. cursor movement and mid-line editing, ctrl+a/e/w/u/k, del, and tab completion. the enabling change was making the console's `\b` non-destructive like an actual terminal, which meant giving it a shadow buffer of the text on screen so the block cursor can sit on a character and put it back afterwards. new commands: `poweroff` (so you stop killing qemu), `date` off the cmos clock, `hexdump` that checks the page tables before reading, `kill`, `history` and `time`. the rtc's decoding is split from its io and tested -- bcd, the pm bit hiding in the top of the hour byte, and 12am being hour zero are each their own small trap.
- **0.0.14** — symbolized backtraces. `tools/gensyms.py` turns the kernel's own `nm` output into a table baked into a `.ksyms` section, and panics, exception dumps and double faults all print a symbolized call chain. the section sits after `.text` so folding it in can never move a function, and the build verifies that rather than trusting it. found two things on the way: our `backtrace()` was colliding with glibc's in the host tests and being silently shadowed (now `kbacktrace`), and the test binaries had no prerequisite on the kernel sources they `#include`, so they were happily running against stale builds.
- **0.0.13** — a tss at last, with an IST stack for the double fault vector. that turns stack overflow from a silent triple-fault reboot into a report naming the thread and its guard page, because the cpu can always find a good stack for that vector even when `rsp` is in the hole. and the pmm now reclaims limine's memory (~1 MiB): the shell moved onto its own pmm-backed thread so the boot thread can exit and stop standing on limine's stack, and the bitmap grew to cover the reclaimable regions, which sit above the last usable one and were previously off the end of the map entirely. new test suite for the tss descriptor encoding, which scatters a base address across two qwords and fails silently when you get it wrong.
- **0.0.12** — kprintf learned the `-` (left justify) flag, which it had been claiming to support by virtue of gcc's format checking without ever implementing. the vmm's boot log used `%-7s`, so the specifier printed literally, every following argument landed in the wrong slot, and the kernel read `__data_end` as a string and page faulted. added `tools/checkfmt.py` to `make test` so no format string can outrun the formatter again.
- **0.0.11** — our own page tables. four levels built at boot, direct map in 2MiB pages, kernel mapped per-section with W^X, NX enabled properly via EFER (and treated as a runtime capability, since a hardcoded NX bit faults on a cpu that lacks it), CR0.WP set so read-only means read-only even in ring 0. `vmm_init` verifies the whole thing by walking its own tables in software -- including the current stack -- before daring to load cr3. guard pages under every thread stack, which needed 2MiB page splitting to punch a hole in the direct map. exception dumps now name the thread that died and say when the address is a guard page. new shell commands: `vmm` to look up any address, `smash` to run off the end of the stack on purpose. 40-odd host assertions for the page table code, because a mistake there is a triple fault with nothing to read.
- **0.0.10** — milestone 7. serial input on irq4, with a translation layer for the terminal dialect (cr means enter, del means backspace, `ESC[A` means up) so the shell is drivable over the wire. keyboard and serial now feed one shared input queue in `drivers/input.c` instead of the keyboard owning the buffer privately. six host test suites moved into `tests/` behind `make test`, plus `tools/boottest.sh` which boots the iso and types at it. github actions runs the lot on every push. panics can now be escaped over serial too, not just from the keyboard.
- **0.0.9** — the shell grew the things you immediately miss when you sit down at it. the keyboard driver now decodes ctrl as a modifier (ctrl+letter arrives as a control code, so ctrl+c is 3) and stops throwing away the e0-prefixed arrow keys, which meant widening the ring buffer to 16 bits so arrows cant be mistaken for characters. on top of that: 16 lines of command history on up/down, ctrl+c to abandon a line and recall running personas, and a panic you can escape -- it polls the 8042 by hand and resets on any keypress instead of halting forever and making you kill qemu. keyboard and shell tests grew to 20 and 32 cases.
- **0.0.8** — milestone 6. an interactive shell with line editing and nine commands, running as a real thread (the boot thread renames itself `shell` and takes the job). a waitq in the scheduler plus a blocking `keyboard_getchar_blocking()`, so the prompt costs nothing while it waits instead of spinning on hlt. `summon` spawns persona threads on demand, which replaces the m5 demo threads that used to print forever and made the console unusable. the `FAULT_DEMO` build flag is gone -- the `crash` command does the same job better, and from thread context rather than early boot. shell parsing and dispatch are host-tested (20 cases, incl. argv clamping and empty lines).
- **0.0.7** — milestone 5. the pit ticks at 100hz on irq0 with a global tick counter and uptime. threads: kernel stacks from the pmm, a fabricated initial stack so a brand new thread can be "resumed" into existence, and a 16-instruction context switch in asm. preemptive round-robin scheduling on a 50ms quantum, blocking `sleep_ms()`, `thread_exit()` with a reaper that frees dead threads' stacks (from a different thread's stack, which is the only safe way). an idle thread that hlts so theres always somebody to hand the cpu to. the allocators and kprintf take interrupts down while they work, since a half-updated free list is nobodys friend. demo: pixie and jack-frost count at different rates, the herald says its piece and dies to give the reaper something to do, and typing still works throughout. the context switch is host-tested, including whether all six callee-saved registers actually survive a round trip.
- **0.0.6** — milestone 4. physical memory manager: parses limine's memory map (and prints it at boot), bitmap over every 4k frame, contiguous multi-page allocation with a rotating search hint, stats. kernel heap on top: first-fit free list, 16-byte aligned payloads, magic-guarded headers that catch double frees and wild pointers, address-ordered coalescing, grows by whole pages from the pmm. boot runs a self-test over both (8 frames + 5 heap blocks, pattern verified, freed out of order, books must balance) and panics if anything is off. tested on the host too, 25 assertions incl. draining ram dry and checking no frame is ever handed out twice.
- **0.0.5** — milestone 3. 8259 pic remapped to vectors 32-47 with spurious irq filtering, an irq_register() layer so drivers can claim lines, and a ps/2 keyboard driver: scancode set 1 -> ascii, shift + capslock state (they cancel, as the gods intended), e0 prefixes swallowed, all landing in a ring buffer. the scancode state machine is split from the irq handler and tested on the host (11 scenarios). boot now ends at a prompt that echoes thy keystrokes, live.
- **0.0.4** — milestone 2. our own gdt (tss slot reserved), idt with 256 macro-generated isr stubs, and an exception handler that prints the vector name, decoded page fault info (cr2 + error bits) and a full register dump before panicking. at the time this shipped with a `FAULT_DEMO=1` build flag to trigger it; as of 0.0.8 thats the shell's `crash` command instead. the kernel also now boots, panics and (eventually) reboots with the appropriate persona social link ceremony. thou art I, and I am thou.
- **0.0.3** — milestone 1 done. kprintf (with actual tested number formatting), framebuffer console with the spleen 8x16 font, glyph blitting, scrolling, block cursor, and panic(). boot banner shows up on screen and serial at the same time. the temporary decimal-printer hack from 0.0.2 is gone, unmourned.
- **0.0.2** — com1 uart driver (polled, 115200 8n1, with loopback self test). framebuffer request to limine, boot info logged over serial, test pattern on screen. run `make run` and watch the serial chatter in your terminal.
- **0.0.1** — project scaffold. limine v9.x boots a stub kernel that halts politely. build system, linker script, license, this readme.
