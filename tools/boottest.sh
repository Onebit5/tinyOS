#!/bin/sh
# boot the iso headless and actually use the thing: type commands at the
# shell over the serial line and check the answers come back.
#
# this is only possible because com1 is wired to the input queue, so the
# shell cant tell the difference between a keyboard and a pipe.
#
#   usage: tools/boottest.sh [tinyos.iso]

set -eu

ISO="${1:-tinyos.iso}"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

if [ ! -f "$ISO" ]; then
    echo "no such iso: $ISO (run 'make iso' first)" >&2
    exit 1
fi

echo "booting $ISO and driving the shell over serial..."

# the sleeps matter: the kernel has to get all the way to a prompt
# before it can hear us, and each command needs a beat to answer
{
    sleep 8
    printf 'help\r';   sleep 1
    printf 'mem\r';    sleep 1
    printf 'ps\r';     sleep 1
    printf 'uptime\r'; sleep 1
    printf 'echo the bond endures\r'; sleep 1
    printf 'summon pixie\r'; sleep 3
    printf 'ps\r';     sleep 2
    printf 'vmm\r';    sleep 2
} | timeout 60 qemu-system-x86_64 \
        -M q35 -m 2G -cdrom "$ISO" \
        -display none -serial stdio -no-reboot \
        > "$LOG" 2>&1 || true

fail=0
check() {
    if grep -qF "$2" "$LOG"; then
        printf '  ok    %s\n' "$1"
    else
        printf '  FAIL  %s (no "%s" in the log)\n' "$1" "$2"
        fail=1
    fi
}

# did it boot at all
check 'kernel banner'      'tinyOS v'
check 'framebuffer found'  'framebuffer :'
check 'idt armed'          '256 gates armed'
check 'memory map parsed'  'memory map, as declared by limine'
check 'memory selftest'    'books balance'
check 'own page tables'    'cr3 is ours'
check 'W^X applied'        'W^X on .text'
check 'scheduler started'  'the wheel turns'
check 'reached the prompt' 'velvet>'

# did it answer us
check 'help works'    'call forth a persona thread'
check 'mem works'     'physical frames'
check 'uptime works'  'awake for'
check 'echo works'    'the bond endures'
check 'ps works'      'idle'
check 'summon works'  'has answered thy call'
check 'thread ran'    '[pixie]'
check 'vmm works'     'pml4 at'

# and did it stay alive rather than falling over
if grep -qF 'KERNEL PANIC' "$LOG"; then
    echo '  FAIL  it panicked somewhere:'
    grep -A6 'KERNEL PANIC' "$LOG" | sed 's/^/        /'
    fail=1
else
    echo '  ok    no panic'
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo '--- full serial log ---'
    cat "$LOG"
    exit 1
fi

echo 'boot test passed'
