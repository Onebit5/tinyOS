# where this is going

0.1.0 is the point where programs stopped being a demo and became a
thing the kernel actually hosts: their own memory, their own privilege
level, reclaimed when they die.

what follows is a lot of small steps rather than one heroic one. each
line below should be a version on its own -- buildable, bootable, and
worth a paragraph in the changelog.

0.1.x filled in what having processes implies. 0.2.x is about the
assumptions underneath *that* turning out to be too small. 0.3.0 is
where the machine stops being the only one in the room.

## 0.1.x — filling in what having processes implies

**0.1.1 processes, not just threads.** ~~exit codes carried back to
whoever waited. a real process table with a parent. `ps` splitting
kernel threads from programs properly. `kill` learning to end a
program rather than refusing anything blocked -- which needs threads to
carry a back-pointer to the waitq they are parked on, the same gap that
makes `kill` timid today.~~ **done in 0.1.1.**

**0.1.2 more to ask for.** ~~the syscall table is six calls wide. it
wants `open`/`read`/`close` against the ramdisk so a program can read a
file instead of being handed one; `getpid`; `spawn` and `wait` so a
program can start another. that last pair is what makes a userspace
shell possible at all.~~ **done in 0.1.2.**

**0.1.3 input that belongs to somebody.** ~~right now the kernel shell
peeks at keys while a program runs and hopes. a foreground process
should *own* the input queue, with ctrl+c delivered to it rather than
handled on its behalf. that is the beginning of a controlling
terminal.~~ **done in 0.1.3.**

**0.1.4 a userspace toolbox.** ~~`cat`, `echo`, `uptime` as real programs
in `ramdisk/bin` rather than kernel commands. the kernel shell keeps
only what genuinely needs kernel access -- `vmm`, `bt`, `hexdump`, `ps`
-- and everything else moves out. the point is to find out which
commands were secretly using kernel internals.~~ **done in 0.1.4**, and
the answer was: `echo` needed only argv, `cat` and `uptime` needed
nothing that did not already exist, and `ls` needed one new syscall
because `open` can only answer about a name you already know.

**0.1.5 users.** ~~a read-only `passwd` in the ramdisk, a login prompt, a
uid on each process, and syscalls that check it.~~ **done in 0.1.5.** worth saying: this
only means something because ring 3 exists -- without a boundary the
hardware defends, a "user" is a variable that says you are an admin.
persistence is not required for this; only *changing* users needs a
writable disk.

**0.1.6 a kernel that measures itself.** ~~per-process cpu time, so `ps`
grows a cpu% column and the scheduler stops being theoretical. a `top`
that redraws. peak memory. how many syscalls of each kind.~~ **done in
0.1.6.**

**0.1.7 modern interrupt hardware.** ~~acpi tables (the loader hands over the
rsdp), then the lapic and ioapic in place of the 8259, and the lapic
timer in place of the pit. lateral on its own -- the same behaviour on
better hardware -- but it is the prerequisite for more than one cpu.~~
**done in 0.1.7**, with a fallback: if the firmware will not say where
the apics are, the 8259 keeps the job and nothing above notices.

**0.1.8 knowing what is plugged in.** ~~pci enumeration and an `lspci`.
small, satisfying, and the doorway to every real device driver.~~
**done in 0.1.8.**

**0.1.9 allocators worth the name.** ~~the pmm is a linear bitmap scan; a
buddy allocator would make it logarithmic. a slab allocator for the
fixed-size things I allocate constantly (threads, address spaces). a
`kmalloc` that is not first-fit.~~ **done in 0.1.9.**

**0.1.10 a real filesystem, on a real disk.** ~~an ahci driver and fat32, read and write.~~ **done in 0.1.10.**

**0.1.11 one namespace.** ~~the disk becomes the root and the ramdisk
moves to /boot, instead of the disk being bolted on at /disk. a bare
name is looked for on the disk first and the ramdisk second, so a disk
may supply its own copy of anything and a machine without one carries on
exactly as before.~~ **done in 0.1.11.** the ramdisk stays: it is what
makes the machine work when the disk does not, and it is the foundation
for booting one medium to install onto another.

**0.1.12 my own bootloader.** ~~stage 1 and stage 2, filling in the same
structures the kernel expects so the kernel itself need not change.~~
**done in 0.1.12.** philemon: one file, whose first 512 bytes are the
only part the bios will read, and a 64-bit half in C. limine is gone
entirely, along with the iso, the uefi path and the boot protocol that
came with it. the kernel is handed one struct in rdi and knows nothing
about anybody's protocol including mine.

## 0.2.x — the kernel stops being one thing at a time

0.1.x was about a kernel that hosts programs. 0.2.x is about the
assumptions underneath that turning out to be too small: one cpu, one
directory, one architecture, one process at the front of everything.

**0.2.0 more than one cpu.** ~~the firmware has been telling me how many
there are since 0.1.7 and I have been ignoring all but the first. the
other cores wake in real mode at a page-aligned address below a
megabyte, so this needs a second small trampoline out into long mode --
philemon just taught me how to write one, and this time the budget is
four kilobytes rather than 512 bytes. per-cpu state to go with it:
`current`, the idle thread, the tss and the gdt stop being globals.
one enormous lock around the kernel to begin with, because a wrong
answer that is slow is still an answer.~~ **done in 0.2.0**, except for
the lock -- the woken cores halt instead. giving them kernel code to run
before 0.2.1 has replaced the thirty-nine places that call `cli` mutual
exclusion would not be a slow answer, it would be a corrupt one.

**0.2.1 locks worth the name.** ~~there are 39 `irq_save` pairs across
twelve files and every one of them is a lie on a second core: turning
interrupts off here says nothing about a thread running there. this is
the audit -- spinlocks, and then going through all 39 deciding what each
one was ever protecting. some become locks, some become per-cpu data,
and some turn out never to have needed anything. the fat32 driver has a
single 512-byte scratch buffer shared by every call into it, which is
not a race so much as a promise of one. the allocators get a host test
that hammers them from several threads at once, since that is the only
way I can see these bugs at all.~~ **done in 0.2.1.** the rank rule
complains rather than panics for now: with one core running kernel code
a wrong order cannot deadlock anything, so it should cost a line of text
and not a working machine. it becomes fatal in 0.2.2.

**0.2.2 a scheduler on every core.** ~~a run queue per cpu instead of one
behind a lock, work moved between them when they drift apart, and tlb
shootdown -- when one core unmaps a page the others still have it
cached, and nothing in the hardware tells them. that takes an
inter-processor interrupt and a handshake, and it is where real kernels
have real bugs. `ps` gains a column for which core, and `top` a row per
core.~~ **done in 0.2.2**, with one deliberate difference: **one shared
run queue that every core picks from**, rather than a queue each with
migration between them. at four cores the lock is not the bottleneck,
and an idle core taking whatever is ready is load balancing already --
without the machinery that per-core queues then need in order to undo
what they took apart. worth revisiting when there is evidence the lock
is the thing in the way.

**0.2.3 somewhere to stand.** ~~the shell has no idea where it is. `cd`,
`pwd`, a working directory per process, and paths resolved relative to
it -- `..` included, which the vfs currently throws away. `mkdir` and
`rmdir` to go with it.~~ **done in 0.2.3.**

**0.2.4 commands that are just commands.** ~~`run bin/cat` is an
embarrassment left over from when running a program was the
demonstration. a program should be typed by its name. the shell learns
where to look, `run` survives only for saying explicitly what to run,
and the two lists in `help` become one.~~ **done in 0.2.4.**

**0.2.5 arguments worth parsing.** ~~every program parses argv by hand
and none of them agree. a small getopt in the user library: `-v`,
`--verbose`, clustering, `--` to stop parsing, and a usage string each
program declares once and never writes out twice.~~ **done in 0.2.5.**

**0.2.6 help that knows what it is describing.** ~~because 0.2.5 makes
each program declare its arguments, `help cat` can print them without
anybody writing that twice. `--help` on any program prints the same
thing, from the same place.~~ **done in 0.2.6**, and the shape of the
answer settles the question: `help cat` *runs* cat with `--help`, so
there is no second copy anywhere that could drift. the plain `help`
became names in columns, since a description beside every one of them
was a wall you had to read all of to find the line you wanted.

**0.2.7 making and unmaking.** ~~`rm`, `cp`, `mv`, `touch`, and files
that remember when they were written. fat has fields for all of that
and I have been writing zeroes into them.~~ **done in 0.2.7.** the two
things worth writing down: `mv` moves a name and not a file, so moving
a hundred megabytes costs the same as moving nothing -- and `rename`
writes the new entry before striking out the old one, deliberately, so
that a machine dying between the two leaves a file with two names
rather than none. `stat` came with it, because `ls -l` wanted a size
and a date for every name in a directory and opening each one to find
out would be a descriptor apiece for what the directory entry already
said.

**0.2.8 pipes.** ~~`cat x | head` has been an error message since
0.1.10. a pipe is a descriptor with a buffer behind it, a reader that
blocks until there is something or the writer is gone, and two
processes wired together at spawn. then `head`, `wc`, `grep` and
`sort`, which are only worth having once there is something to connect
them to.~~ **done in 0.2.8.** the buffer was the easy half. the two
rules hanging off the reference counts are the whole thing: a read
returns 0 when the last writer goes, which is the only reason a
pipeline ever finishes, and a write fails when the last reader goes,
which is the only reason `cat huge | head` stops instead of blocking
forever. unix raises SIGPIPE there and the default is to die; with no
signals I do the dying part directly. builtins cannot be in a pipeline
and it says so -- the shell is a kernel thread printing straight at the
screen, so it has no stdout to hand anybody.

**0.2.9 an editor, and redirection.** ~~something nano-shaped, so a file
on the disk can be changed by the machine that stores it rather than by
rebuilding the image. the first program that has to think about a screen
rather than a stream.~~ **done in 0.2.9.** the editor is `margaret`,
after the one who keeps the compendium -- the only thing in the velvet
room that is written down.

it needed four syscalls nothing had wanted before: one key rather than a
line, the size of the screen, where to put the cursor, and clear. asking
for a key *is* raw mode -- there is no flag anywhere saying a terminal
is raw, because asking for a line and asking for a key are different
questions and the answer to each is obvious.

`write` is gone with it. `echo hello > file.txt` says the same thing
with punctuation everybody already knows, and it is one program fewer.
that took the change 0.2.8 said it was deferring: **0, 1 and 2 became
real descriptors**. they had never been entries in the table at all --
the syscall layer answered them directly, because until pipes there was
exactly one place each could point -- and pointing stdout at a *file*
has nowhere to be written down until they are slots like any other.
`>`, `>>` and `<`, per command rather than per line, so `sort < a > b`
is one stage with both ends moved.

**0.2.10 job control.** ~~ctrl+z, `bg`, `fg`, `jobs`, and process groups
underneath them -- which the terminal half-knows about already, since it
has had a foreground process since 0.1.3.~~ **done in 0.2.10.** stopping
a thread turned out to want a *flag* rather than a state: a suspended
thread may also be blocked on a pipe or asleep, and those answer
different questions -- "what is it waiting for" against "may it run at
all". squeezing both into one enum means a stopped thread forgetting it
was stopped the moment anybody wakes it.

the terminal talks to a group now rather than a pid, because `cat x |
wc -l` is two processes and one thing the person typing is thinking
about. and with no signals, a suspended job announces itself by leaving
a note where the shell will look: waiting has a second way to finish.

**0.2.11 fork, and copy on write.** ~~spawning is the only way to make a
process and it builds one from a file every time. `fork` copies an
address space instead -- or rather does not copy it, marks every page
read-only in both, and copies one page at a time as somebody writes.
the page fault handler stops being purely an error path.~~ **done in
0.2.11.** the awkward part was not the page tables, it was the two
returns: a forked child has to come back from a `syscall` it never
made, holding everything its parent held -- and half of that is in
callee-saved registers that the abi says are somebody else's problem,
sitting in the cpu at the moment of the call and buried under a C
prologue a moment later. so the entry stub writes the whole of ring 3
down on every call now, six extra pushes, for one caller.

the other thing worth writing down: the *parent's* pages have to lose
their write bit too. protecting only the child gives you a fork where
the parent quietly writes through the child's memory, and it looks
like it works.

**0.2.12 demand paging.** ~~if a fault can mean "copy this page" it can
mean "there was never a page here yet". stacks that grow, `mmap`, and
programs that start faster because nothing is loaded until it is
read.~~ **done in 0.2.12.** the whole of it hangs on one thing: a
*record* saying which addresses are legitimately empty. without one
there is no telling a stack that wants to grow from a program
dereferencing nonsense, and a kernel that guesses wrong either kills
good programs or conjures memory for bad ones.

so each space keeps a small table of ranges it has agreed to, and every
not-present fault is answered out of it or not at all. the stack is a
megabyte of range with two pages in it; the address below is nothing,
which is the guard page for free -- it costs no memory because there is
nothing there to cost anything.

lazy program loading is for images that will still be in memory when
the program runs, which is the ramdisk and therefore every program
there is. one read off the disk is a copy on the heap somebody has to
free, and making it outlive an unknown number of forks is a lifetime
scheme demand paging does not need in order to be worth having.

**0.2.13 a cache between the disk and everything else.** ~~every read
goes to the drive today, one sector at a time, through a single bounce
buffer. a cache of blocks, dirty ones written back later, and `sync` to
mean it.~~ **done in 0.2.13.** it slots exactly where fat32's two
function pointers already were, which is the whole reason it could be
added without the filesystem knowing: fat32 was handed a way to move
sectors and it is still handed a way to move sectors.

the write-back half is a promise broken on purpose. until a sync, what
is on the disk is not what the machine believes -- which is why unix
has had the command since 1971, why reboot and poweroff call it, and
why there is a flusher on a timer to turn "you might lose anything"
into "you might lose the last few seconds".

**0.2.14 a filesystem with opinions.** ~~fat records no ownership and no
permissions, which is why everything on the disk is 0644 by decree. a
filesystem that has them -- ext2, or one of my own -- plus symlinks and
proper timestamps. the vfs finally has two things to be a layer
over.~~ **done in 0.2.14.** ext2, read and written: superblock, block
groups, bitmaps, inodes, and the twelve-direct-then-indirect block map
that every unix filesystem of the era used.

the difference from fat is one sentence: **a name and a file are
different objects**. in fat a file *is* its directory entry, so it has
exactly one name and ownership has nowhere to live. here a directory
entry points at an inode, which is why permissions belong to the file
rather than to the name, why a rename moves nothing, and why `chmod`
finally has somewhere to write its answer.

there is no e2fsck on this machine, so the formatter and the driver
would otherwise be two programs by one author agreeing with each other.
tools/readext2.py is the answer: a reader written from the on-disk
layout, run by the test target *after* the driver has finished writing
to the image. it found two real bugs before the driver existed.

**0.2.15 partitions.** ~~a disk is not a filesystem; it is a table
saying where several of them are. mbr and gpt, and mounting by which
partition rather than by which drive answered first.~~ **done in
0.2.15.** both tables, because a machine has to read both: mbr's four
sixteen-byte entries from 1983, and gpt's checksummed header and array
-- with the protective mbr a gpt disk carries so an old tool sees a
full disk rather than an empty one.

the crc is the interesting part. a gpt table that does not add up is
*known* to be corrupt and is refused, where a corrupt mbr is simply
followed. so the parser is judged by what it will not do: mounting a
filesystem at an address nobody chose is worse than mounting nothing.

the filesystem is handed a view of one partition rather than of the
drive, so every address it uses is its own and it never finds out it is
not alone. a drive with no table gets one entry covering the whole of
itself -- an image written straight to sector zero is ordinary, and
should not be a special case anywhere above.

**0.2.16 more than one screen.** ~~alt+f1 through f4, several sessions
at once, each with its own foreground process and its own scrollback.
the terminal layer has been one machine pretending to be one seat.~~
**done in 0.2.16.** the console already kept a shadow of every cell --
added in 0.1.x so a block cursor could put back the character it was
sitting on -- and a console nobody is looking at turns out to be
exactly that shadow with nothing painting it. most of the driver is the
code it always was with one question in front of the parts that touch
pixels.

the harder half was the shell. its state was file-static, which was
correct while there was one of it and became a bug the moment there
were four: four shells sharing one working directory is one shell with
four windows onto it. it is a session per console now, reached through
the calling thread rather than passed in.

and one rule the whole thing turns on: **output belongs to its writer,
input belongs to the screen**. a shell on console 2 printing while
console 1 is displayed must not scribble over console 1; a process at
the front of console 3 is at the front of console 3 and is still not
being typed at.

**0.2.17 something to point with.** ~~the ps/2 mouse, a cursor, and
whatever it turns out to be good for. the first input that is not a
stream of characters.~~ **done in 0.2.17.** what it turned out to be
good for is what a pointer has been good for on a text console since
gpm in 1993: drag over a path in an `ls`, press the middle button at a
prompt, and it is typed for you. the characters go into the input queue
as though somebody had pressed the keys, so nothing above knows a mouse
exists -- the shell's line editor cannot tell and does not have to.

and it really is a different shape of input. a queue is right for
typing because typing *is* a sequence, and the order is the meaning. a
mouse reports a change since last time and the interesting thing is
never one report, it is where the pointer ended up -- so the driver
keeps a position and the events are edges.

the one hard part is that the 8042 has no framing. a packet is three
bytes and nothing marks where one starts except a bit that is always
set, so a single dropped byte puts every packet after it one out of
step and the pointer flies off in a straight line. that looks like a
hardware fault and is not, which is why the count of discarded bytes is
something `mouse` prints.

**0.2.18 variables, and scripts.** ~~an environment inherited across
spawn, `$PATH` meaning what it means everywhere else, and a shell that
can read a file of commands with `if` and `while` in it.~~ **done in
0.2.18.** the environment is one block of "NAME=value" strings on the
process, which is the shape it is for a reason: inheriting is one
memcpy, and inheriting is most of what an environment is *for*. a table
of pointers would need every one of them rewritten on the way into a
child.

`$PATH` means what it means everywhere else now, including for
completion -- which had been walking a fixed list that happened to be
the same one, and stopped being the same one the moment PATH became a
variable that did something. completion offering a program that cannot
be run is worse than no completion: it is completion that lies.

the script syntax ends blocks with `end` rather than `fi` and `done`,
deliberately. borrowing sh's spellings would claim a compatibility that
does not exist -- no functions, no arithmetic, no `&&`, no quoting to
speak of -- and a script that looks like sh and is not is worse than
one that plainly is not. a condition *is* sh's rule, because it is the
right one: a command, true when it exits zero, which makes every
program on the machine a usable condition without any of them knowing.

**0.2.19 an init worth the name.** ~~the shell is started by `kmain`
because there was nothing else to start it. a first process that owns
the others, brings things up in an order, restarts what dies, and shuts
the machine down tidily.~~ **done in 0.2.19.** the four clauses turn out
to be one job seen from four sides, and the one that pays for the rest
is *restarts what dies*: `logout` can end a session now instead of
calling `login` from inside it, so the next person does not inherit the
last one's directory, history, jobs and variables.

the rule with teeth is the one every init has had since sysvinit:
something that dies instantly and is restarted instantly is a machine
that does nothing else ever again. five deaths inside ten seconds and
init leaves it down and says so. the *window* matters as much as the
count -- five deaths across an afternoon is five people logging out, and
a machine that gave up on a console for having been used would be
worse than one with no rule at all.

shutting down is where owning things stops being theoretical. `reboot`
used to sync the disk and reset from whichever console typed it, while
three other sessions carried on writing -- so what reached the drive was
whatever was dirty at the instant somebody asked. stopping everything
first is not something a command can do to itself.

and pid 1 is not decoration: reparenting needs a number that is known
before the process it names exists. a child whose parent dies becomes
init's, and init collects it -- which replaced a sweep that ran on every
spawn and took *every* finished process with it, background jobs whose
exit codes nobody had read yet included.

**0.2.20 the x86 parts, in one place.** ~~everything that assumes this
architecture is scattered through the tree. an `arch/` boundary, drawn
from the outside in, so the rest of the kernel stops naming registers it
has no business knowing about. no new behaviour at all -- the test is
that nothing changes.~~ **done in 0.2.20.** drawn from the outside in
turned out to be the whole instruction: the headers are named for what
the kernel *wants* -- may interrupts happen, stop until something
occurs, this mapping is stale, land the kernel here when this thread
traps -- rather than for what x86 provides. drawn the other way round it
would have produced a `write_cr3()`, which is an x86 instruction wearing
a portable-looking name and worse than the inline asm it replaced,
because the inline asm at least admitted what it was.

the biggest single leak was not the assembly, it was `cpu/interrupts.h`:
nineteen files across mm, sched, fs, drivers and lib included an x86
header to get four lines of interrupt masking, and got a struct listing
rax through r15 and the whole 8259 along with them.

and the version's real deliverable is `tools/checkarch.py`, because a
boundary is worth exactly what checks it. it fails the build on inline
assembly outside `arch/` and on any portable file naming x86, and it
prints the ten files still allowed to -- with the reason for each. the
list is meant to shrink; three of the entries I first wrote turned out
to be wrong, which the checker said so on its first run.

"nothing changes" is checkable and was checked: 1057 of the 1073
functions in both builds have byte-identical instruction sequences, and
every one of the sixteen that differ is accounted for. two of them are
not changes at all -- two files each have a static function called
`mine`, and comparing by name compares the pair.

the thing that nearly slipped through: this kernel builds at `-O0`,
where a plain `static inline` is a call like any other. for `cpu_relax`
that is merely slower; for `cpu_frame_pointer` it is *wrong*, because a
called function reads its own frame and the backtrace would have started
one line low, politely reporting itself.

**0.2.21 a second architecture.** aarch64 on qemu's virt board: a
different uart, a different interrupt controller, a different timer, a
different mmu, and the same kernel above all four. the point is not that
anybody needs tinyOS on arm. the point is that the boundary drawn in
0.2.20 is either real or it is decoration, and this is the only way to
find out.

**0.2.22 a live mode, and an installer.** the ramdisk has been kept
since 0.1.11 on the argument that it is what makes the machine work
when the disk does not. this is the other half of that argument: boot
from one medium, partition and format another, copy the system onto it,
and leave a machine that boots on its own. everything from 0.2.14 and
0.2.15 exists to make this the small step it ought to be.

## 0.3.0 — the machine stops being alone

**0.3.0 local networking.** no internet and nothing routed -- just this
machine and whatever else is on the wire. a network card found the same
way the disk was, then arp, ipv4 and icmp, so it can be pinged and can
ping back. then udp, and enough of a socket layer for a program to use
it. `ifconfig`, `ping`, and something that lists what else is out there.
