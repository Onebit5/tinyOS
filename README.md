# tinyOS

![ci](https://github.com/Onebit5/tinyOS/actions/workflows/ci.yml/badge.svg)

a tiny 64-bit hobby kernel for x86_64, written in C, booted with [Limine](https://github.com/limine-bootloader/limine).

im building this to actually understand what happens between "power button" and "shell prompt". its not trying to be the next linux, its trying to fit in my head.

**version: 0.0.11** (the kernel builds and runs on its own page tables now. W^X on the kernel image, NX everywhere it belongs, and a guard page under every thread stack so running off the end faults cleanly instead of eating the neighbours)

## scope

roughly in order, this is where the project is going:

- [x] boot into 64-bit long mode via limine
- [x] serial (com1) logging
- [x] framebuffer console with its own font rendering
- [x] gdt/idt, real exception dumps instead of silent triple faults
- [x] ps/2 keyboard driver (interrupt driven, no polling)
- [x] physical page allocator + kmalloc heap on top
- [x] pit timer + preemptive round-robin scheduler with kernel threads
- [x] a small interactive shell (help, mem, uptime, ps, the classics)
- [x] ci that builds the iso and boot-tests it in qemu on every push
- [x] our own page tables: W^X, NX, guard pages under thread stacks

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
make test   # run the host test suites (no qemu needed, takes a second)
make boottest   # boot the iso and drive the shell over serial
```

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
mem       frames and heap, honestly counted
uptime    how long since the bond was formed
ps        the threads that walk this realm
summon    call forth a persona thread
vmm       what the page tables say about an address
crash     tempt fate with a wild pointer
smash     run off the end of the stack on purpose
reboot    sever the bond and begin anew
```

`vmm` with no argument points at one thing of each kind -- code, a string constant, the heap, your stack, and an address nobody lives at -- so you can read the permission column and see W^X actually holding. give it a hex address to look that up instead.

line editing: **backspace** deletes, **up/down** walk through the last 16 commands, **ctrl+c** abandons the line you're typing (and recalls any personas that are currently running). adjacent duplicates and empty lines dont make it into the history.

`summon pixie` and `summon jack-frost` spawn real kernel threads that count in the background while you keep typing -- that is the whole scheduler demo in one command. they speak eight times and then depart, which also gives the reaper something to clean up (watch `ps` before and after). they print over the top of your prompt while they run, which looks messy and is entirely honest: three threads are sharing one console and nobody is arbitrating.

cancelling them with ctrl+c is cooperative, not forceful -- we have no signals and no safe way to yank a sleeping thread off the run queue, so a persona notices it has been recalled the next time it wakes up. that can be up to one sleep period later.

`crash` dereferences `0xdeadbeef` on purpose, which page faults inside the shell thread and gets you the full m2 exception report -- decoded fault reason, cr2, every register, then the panic. the machine is dead at that point, but the panic handler polls the 8042 directly (interrupts are never coming back, so the keyboard driver is no help) and any keypress resets the box. it ignores key *releases*, otherwise letting go of the enter key you used to type `crash` would reboot instantly.

## testing

most of this kernel can be tested without booting anything, because the parts that think are deliberately kept separate from the parts that touch hardware. `pmm_init_from_map()` takes a memory map rather than asking limine for one, `keyboard_feed()` takes a scancode rather than reading port 0x60, `serial_feed()` takes a byte, `run_line()` takes a string. so the test suites compile the *real* kernel sources as ordinary linux programs and poke at them:

```
$ make test
  checkfmt   ok        every format string vs what kprintf implements
  kprintf    ok        formatting vs the real printf, 33 cases
  mm         ok        pmm + heap, incl. draining ram dry
  vmm        ok        page tables built and walked, 40+ cases
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

we still dont reclaim the bootloader-reclaimable regions, and now for a sharper reason than before: the stack this all runs on lives there. freeing it would be the last thing this cpu ever did.

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
