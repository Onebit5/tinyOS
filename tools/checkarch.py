#!/usr/bin/env python3
"""does the arch boundary actually hold?

0.2.20 drew a line: everything under kernel/src/arch/x86_64/ assumes this
machine, and everything above it is not supposed to. a line like that is
worth exactly as much as whatever checks it -- it decays the first time
somebody needs a `hlt` in a hurry, and it decays silently, because the
kernel goes on building and booting perfectly.

so this is the thing that notices. it fails the build on:

  * inline assembly outside kernel/src/arch/
  * an #include of arch/x86_64/... from outside kernel/src/arch/, unless
    the file is on the list below

and it prints the list every time, because the list is the interesting
output. it is a record of what has not been done yet, and it is meant to
shrink -- a version that adds a name to it should have said why.

the same idea as tools/checkfmt.py, and for the same reason: the bug it
prevents is one that hides.
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
KERNEL = os.path.join(ROOT, "kernel", "src")
ARCH = os.path.join(KERNEL, "arch")

# files that may still name the architecture, and why.
#
# every one of these is a driver for a chip that is only ever found on a
# pc. they are not moved into arch/x86_64/ because a boundary drawn
# around drivers before there is a second machine to draw it against is a
# guess -- 0.2.21 is an aarch64 port and it is the thing that says which
# of these are "an x86 driver", which are "a driver that happens to use
# port io", and which are neither.
#
# what this list buys in the meantime is that the leak is *counted*
# rather than assumed.
ALLOWED = {
    "drivers/pit.c":      "the 8254 timer, at ports 0x40-0x43 since 1981",
    "drivers/keyboard.c": "ps/2, which is an 8042 at port 0x60",
    "drivers/mouse.c":    "the same 8042, sharing the same port",
    "drivers/rtc.c":      "the cmos clock, at ports 0x70/0x71",
    "drivers/serial.c":   "an 8250 uart -- the chip is not x86, reaching it by port is",
    "drivers/pci.c":      "configuration space through ports 0xcf8/0xcfc",
    "main.c":             "kmain brings the machine up, so it names the machine",
    "shell/shell.c":      "`cpus`, `lspci` and `ioapic` report on this hardware by name",
    "mm/vmm.c":           "the nx bit lives in an msr",
    "mm/addrspace.c":     "tlb shootdown is an inter-processor interrupt",
}

ASM = re.compile(r'\basm\s+volatile\b|\b__asm__\b|\basm\s*\(')
INCLUDE_ARCH = re.compile(r'^\s*#\s*include\s*"(arch/[^"]+)"', re.M)


def sources():
    for base, dirs, files in os.walk(KERNEL):
        dirs[:] = [d for d in dirs if d != "arch"]
        for name in sorted(files):
            if name.endswith((".c", ".h")):
                full = os.path.join(base, name)
                yield full, os.path.relpath(full, KERNEL).replace(os.sep, "/")


def main():
    problems = []
    naming = {}

    for full, rel in sources():
        with open(full, encoding="utf-8", errors="replace") as f:
            text = f.read()

        # inline asm, anywhere outside arch/. no exceptions and no list:
        # this is the whole of what "stops naming registers" means, and a
        # single allowed one is a precedent rather than a line
        for n, line in enumerate(text.splitlines(), 1):
            code = line.split("/*")[0].split("//")[0]
            if ASM.search(code):
                problems.append(
                    "%s:%d: inline assembly outside kernel/src/arch/\n"
                    "    %s\n"
                    "    -> it belongs behind arch/cpu.h, arch/irq.h, "
                    "arch/mmu.h or arch/machine.h" % (rel, n, line.strip()))

        named = sorted({h for h in INCLUDE_ARCH.findall(text)
                        if h.startswith("arch/x86_64/")})
        if named:
            naming[rel] = named

    for rel, headers in sorted(naming.items()):
        if rel not in ALLOWED:
            problems.append(
                "%s: includes %s from outside kernel/src/arch/\n"
                "    -> use the portable header, or add it to ALLOWED in "
                "tools/checkarch.py with a reason" % (rel, ", ".join(headers)))

    stale = sorted(set(ALLOWED) - set(naming))
    for rel in stale:
        problems.append(
            "%s: on the ALLOWED list in tools/checkarch.py and does not "
            "name the architecture any more\n"
            "    -> take it off the list. a list nobody prunes stops "
            "meaning anything" % rel)

    if problems:
        print("arch boundary: %d problem%s"
              % (len(problems), "" if len(problems) == 1 else "s"))
        for p in problems:
            print("  " + p)
        return 1

    clean = sum(1 for _ in sources()) - len(naming)
    print("  checkarch  ok        %d files portable, %d still name x86:"
          % (clean, len(naming)))
    for rel in sorted(naming):
        print("               %-20s %s" % (rel, ALLOWED[rel]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
