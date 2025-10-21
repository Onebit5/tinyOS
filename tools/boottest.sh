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
    printf 'bt\r';     sleep 2
    printf 'date\r';   sleep 1
    printf 'history\r'; sleep 1
    printf 'ls\r';      sleep 2
    printf 'echo the bond endures\r'; sleep 2
    printf 'uptime\r';  sleep 2
    printf 'cat motd.txt\r'; sleep 2
    printf 'arcana\r';  sleep 1
    printf 'persona\r'; sleep 2
    printf 'run bin/hello\r'; sleep 6
    printf 'run bin/counter &\r'; sleep 1
    printf 'run bin/counter &\r'; sleep 3
    printf 'ps\r';     sleep 4
    printf 'run bin/fail\r'; sleep 2
    printf 'run bin/reader\r'; sleep 2
    printf 'run bin/parent\r'; sleep 3
    printf 'run bin/ask\r'; sleep 2
    printf 'Igor\r';  sleep 2
    printf '\003';    sleep 2
    printf 'ps\r';     sleep 1
    printf 'dmesg\r';  sleep 2
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
check 'framebuffer found'  'framebuffer :'   # serial keeps everything
check 'idt armed'          '256 gates armed'
check 'memory map parsed'  'memory map, as declared by limine'
check 'memory selftest'    'books balance'
check 'own page tables'    'cr3 is ours'
check 'W^X applied'        'W^X on .text'
check 'tss loaded'         'tss loaded'
check 'boot thread left'   '[boot] hath returned'
check 'memory reclaimed'   'reclaimed'
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
check 'bt works'      'call trace:'
check 'symbols work'  'shell_run+'
check 'date works'    ':'
check 'history works' 'uptime'
check 'ramdisk mounted' 'ramdisk    :'
check 'ls is a program'  'bin/ls is pid'
check 'ls works'         'motd.txt'
check 'echo is a program' 'bin/echo is pid'
check 'echo works'       'the bond endures'
check 'uptime works'     'awake for'
check 'cat works'     'Thou art I'
check 'arcana works'  'COMPUTER ARCANA'
check 'persona works' 'velvet@tinyOS'
check 'ring 3 reached' 'entered ring 3'
check 'userspace ran'  'A voice speaks from ring 3'
check 'syscalls work'  'the kernel yet lives'
check 'loop completed'  '5 ... the kernel yet lives'
check 'registers kept'  'My purpose is fulfilled'
check 'program exited'  'sea of souls'
check 'two at once'     '(background)'
check 'isolated memory' 'this memory is mine alone'
check 'ps has processes' 'processes'
check 'pids assigned'    'is pid'
check 'exit code kept'   'exited with 42'
check 'open/read work'   'in bites of 32'
check 'spawn works'      '[parent] it is pid'
check 'wait works'       'exactly as foretold'
check 'a program reads'  'what is thy name?'
check 'input reaches it' 'well met, Igor'
check 'ctrl+c delivered' 'leaving politely'
check 'dmesg works'    'cr3 is ours'

# and did it stay alive rather than falling over
if grep -qF 'refused a pointer' "$LOG"; then
    echo '  FAIL  a syscall refused a program its own memory:'
    grep -m3 'refused a pointer' "$LOG" | sed 's/^/        /'
    fail=1
else
    echo '  ok    no pointer refused'
fi

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
