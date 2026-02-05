# tinyOS build. needs gcc, nasm, make. `make run` additionally needs qemu.
#
# targets:
#   make        -> kernel elf in bin/
#   make run    -> boot it in qemu (CPUS=4, MEM=512M, QEMU_EXTRA=...)
#   make clean

KERNEL := tinyos

# the bootable image, and the default target. defined up here with the
# other names because a prerequisite is expanded where it is written --
# further down, `all: $(BOOTIMG)` quietly meant `all:` with nothing to do
BOOTIMG := tinyos.img

# knobs for `make run`. they have to be make variables rather than extra
# words on the command line, because `make run -smp 4` hands -s -m -p to
# *make* -- and -p means "print the entire database", which is a
# surprising amount of ukrainian
CPUS ?= 1
MEM  ?= 2G
QEMU_EXTRA ?=

CC   := gcc
LD   := ld
NASM := nasm

# freestanding kernel flags. the -mno-* soup is because I cant use fpu/sse
# in the kernel (no context saving yet), and no red zone because interrupts
# would trash it
CFLAGS := -g -Wall -Wextra -std=gnu11 \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIC \
	-m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
	-mcmodel=kernel -Ikernel/src -Iboot -MMD -MP -fno-omit-frame-pointer

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld

NASMFLAGS := -f elf64 -g

CSRC := $(shell find kernel/src -name '*.c')
ASRC := $(filter-out kernel/src/cpu/trampoline.asm, \
          $(shell find kernel/src -name '*.asm'))
OBJ  := $(patsubst kernel/src/%.c,obj/%.c.o,$(CSRC)) \
        $(patsubst kernel/src/%.asm,obj/%.asm.o,$(ASRC)) \
        obj/cpu/trampoline.c.o

.PHONY: all run bootimg clean distclean

all: $(BOOTIMG)

# two passes, because the symbol table describes addresses and linking
# it in changes them. .ksyms sits after .text in the linker script, so
# folding it in shifts .data but cannot move a single function -- and
# gensyms --check proves that held instead of me just hoping.
bin/$(KERNEL): $(OBJ) kernel/linker.ld tools/gensyms.py
	@mkdir -p $(@D) obj
	@python3 tools/gensyms.py --stub > obj/ksyms.c
	@$(CC) $(CFLAGS) -c obj/ksyms.c -o obj/ksyms.o
	@echo '  LD      pass 1 (to find out where everything landed)'
	@$(LD) $(LDFLAGS) $(OBJ) obj/ksyms.o -o $@.pass1
	@echo '  GENSYMS obj/ksyms.c'
	@python3 tools/gensyms.py $@.pass1 > obj/ksyms.c
	@$(CC) $(CFLAGS) -c obj/ksyms.c -o obj/ksyms.o
	@echo '  LD      pass 2 (with the symbols folded in)'
	@$(LD) $(LDFLAGS) $(OBJ) obj/ksyms.o -o $@
	@python3 tools/gensyms.py --check $@ obj/ksyms.c
	@rm -f $@.pass1

obj/%.c.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.asm.o: kernel/src/%.asm
	@mkdir -p $(@D)
	$(NASM) $(NASMFLAGS) $< -o $@

# the code a second cpu wakes up in. it runs in real mode at a fixed low
# address, which is nowhere the linker would put anything, so it is
# assembled flat and carried inside the kernel as bytes
obj/cpu/trampoline.bin: kernel/src/cpu/trampoline.asm
	@mkdir -p $(@D)
	$(NASM) -f bin $< -o $@

obj/cpu/trampoline.c: obj/cpu/trampoline.bin tools/bin2c.py
	@python3 tools/bin2c.py smp_trampoline $< > $@

obj/cpu/trampoline.c.o: obj/cpu/trampoline.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJ:.o=.d)

# ---- userspace ------------------------------------------------------
#
# a ring 3 program is built much like the kernel -- freestanding, no
# libc -- but without -mcmodel=kernel, since it lives at 0x400000 in
# the low half rather than up in the higher one. it gets copied into
# the ramdisk, which is how the kernel finds it.

UCFLAGS := -Wall -Wextra -std=gnu11 -O1 \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-pic \
	-fno-omit-frame-pointer \
	-m64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -Iuser

ULDFLAGS := -nostdlib -static -T user/linker.ld

USER_PROGS := ramdisk/bin/hello ramdisk/bin/counter ramdisk/bin/fail \
              ramdisk/bin/reader ramdisk/bin/parent ramdisk/bin/ask \
              ramdisk/bin/echo ramdisk/bin/cat ramdisk/bin/uptime ramdisk/bin/ls \
              ramdisk/bin/whoami ramdisk/bin/write

ramdisk/bin/%: user/%.c user/syscall.h user/linker.ld
	@mkdir -p $(@D)
	$(CC) $(UCFLAGS) -c $< -o obj/user_$*.o
	$(LD) $(ULDFLAGS) obj/user_$*.o -o $@

# the ramdisk is a plain tar. --format=ustar because thats the one the
# kernel knows how to read, and the flags after it keep the archive
# byte-identical between builds so the iso doesnt churn
RAMDISK := bin/ramdisk.tar
RAMDISK_FILES := $(shell find ramdisk -type f 2>/dev/null)

$(RAMDISK): $(USER_PROGS) $(RAMDISK_FILES)
	@mkdir -p $(@D)
	@# git only tracks the execute bit, so the modes that matter are set
	@# here rather than trusted to the checkout. velvet-room.txt is the
	@# one the kernel refuses to a guest
	@chmod 600 ramdisk/velvet-room.txt
	@chmod 644 ramdisk/passwd ramdisk/*.txt 2>/dev/null || true
	@chmod 600 ramdisk/velvet-room.txt
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime=@0 -cf $@ -C ramdisk .

# ---- philemon, the bootloader ---------------------------------------
#
# there was a borrowed one here until 0.1.12. writing a bootloader and
# then booting with somebody else's is not much of a bootloader, so it
# is gone -- along with the iso, the uefi path and the protocol that
# came with it. the kernel is handed one struct now, in rdi, and knows
# nothing about anybody's boot protocol including mine.

BOOTCC    := gcc
BOOTCFLAGS := -std=gnu11 -ffreestanding -fno-pic -fno-stack-protector \
              -mno-red-zone -mno-sse -mno-mmx -mno-80387 -mcmodel=small \
              -Wall -Wextra -O2 -Iboot

bin/boot/philemon.bin: boot/philemon.asm boot/philemon.inc
	@mkdir -p $(@D)
	$(NASM) -f bin $< -o $@

bin/boot/philemon64.bin: boot/philemon.c boot/philemon.h boot/philemon.ld
	@mkdir -p $(@D) obj
	$(BOOTCC) $(BOOTCFLAGS) -c boot/philemon.c -o obj/philemon.o
	$(LD) -T boot/philemon.ld -nostdlib -static --no-warn-rwx-segments \
		obj/philemon.o -o obj/philemon.elf
	objcopy -O binary obj/philemon.elf $@

$(BOOTIMG): bin/boot/philemon.bin bin/boot/philemon64.bin \
            bin/$(KERNEL) $(RAMDISK) tools/mkboot.py boot/philemon.inc \
            boot/philemon.h
	@python3 tools/mkboot.py $@ bin/boot/philemon.bin \
		bin/boot/philemon64.bin bin/$(KERNEL) $(RAMDISK)

.PHONY: bootimg
bootimg: $(BOOTIMG)

# two drives: the one philemon is on, and the one with the files. the
# boot image goes first and is named as the boot device twice over --
# `order=c` so the bios does not go looking for a floppy it has not got,
# and a bootindex on each so the order is not left to chance.
#
# the kernel does not care which is which: it tries every drive until one
# has a filesystem it recognises, and the boot image has none
#   make run            one cpu
#   make run CPUS=4     four
#   make run MEM=512M QEMU_EXTRA="-d int"
run: $(BOOTIMG) $(DISK)
	qemu-system-x86_64 -M q35 -m $(MEM) -smp $(CPUS) -serial stdio \
		-boot order=c \
		-drive id=boot,file=$(BOOTIMG),format=raw,if=none \
		-device ide-hd,drive=boot,bus=ide.0,bootindex=0 \
		-drive id=data,file=$(DISK),format=raw,if=none \
		-device ide-hd,drive=data,bus=ide.1,bootindex=1 \
		$(QEMU_EXTRA)

# the disk, which is a real filesystem rather than an archive: built by
# tools/mkfat.py out of whatever is in diskroot/, and attached to qemu
# as a sata drive. it is deliberately NOT rebuilt by `make run` once it
# exists -- the whole point is that what you write to it stays written,
# and regenerating it every boot would quietly undo that. `make disk`
# starts over when you want a clean one
DISK := disk.img

$(DISK): tools/mkfat.py $(shell find diskroot -type f 2>/dev/null)
	@python3 tools/mkfat.py $@ diskroot 64

.PHONY: disk
disk:
	@rm -f $(DISK)
	@$(MAKE) --no-print-directory $(DISK)

# ---- host tests -----------------------------------------------------
#
# the testable guts of the kernel are deliberately split from the parts
# that touch hardware: pmm_init_from_map() takes a memory map instead of
# asking the loader, keyboard_feed() takes a scancode instead of reading a
# port, and so on. that lets all of this run as ordinary linux programs.
# TINYOS_HOSTED turns irq_save/irq_restore into no-ops, since userspace
# gets shot for saying cli.

HOSTCC    := gcc
HOSTFLAGS := -std=gnu11 -Wall -Wextra -g -DTINYOS_HOSTED -Ikernel/src -Iboot

TEST_BINS := bin/tests/kprintf bin/tests/mm bin/tests/buddy bin/tests/slab \
             bin/tests/vmm bin/tests/gdt \
             bin/tests/ksyms bin/tests/rtc bin/tests/ramdisk bin/tests/elf \
             bin/tests/addrspace bin/tests/process \
             bin/tests/syscall bin/tests/tty bin/tests/auth bin/tests/acpi bin/tests/pci \
             bin/tests/keyboard bin/tests/serial \
             bin/tests/fat32 bin/tests/vfs bin/tests/philemon \
             bin/tests/shell bin/tests/switch

bin/tests/kprintf:  tests/test_kprintf.c  kernel/src/lib/kprintf.c
bin/tests/mm:       tests/test_mm.c       kernel/src/mm/pmm.c \
                    kernel/src/mm/buddy.c kernel/src/mm/slab.c \
                    kernel/src/mm/kmalloc.c kernel/src/lib/string.c
bin/tests/buddy:    tests/test_buddy.c    kernel/src/mm/buddy.c \
                    kernel/src/lib/string.c
bin/tests/slab:     tests/test_slab.c     kernel/src/mm/slab.c \
                    kernel/src/mm/pmm.c kernel/src/mm/buddy.c \
                    kernel/src/mm/kmalloc.c kernel/src/lib/string.c
bin/tests/addrspace: tests/test_addrspace.c kernel/src/mm/addrspace.c \
                    kernel/src/mm/vmm.c kernel/src/mm/slab.c \
                    kernel/src/lib/string.c
bin/tests/vmm:      tests/test_vmm.c      kernel/src/mm/vmm.c \
                    kernel/src/lib/string.c
bin/tests/ksyms:    tests/test_ksyms.c    kernel/src/lib/ksyms.c
bin/tests/rtc:      tests/test_rtc.c      kernel/src/drivers/rtc.c
bin/tests/process:  tests/test_process.c  kernel/src/sched/process.c \
                    kernel/src/lib/string.c
bin/tests/pci:      tests/test_pci.c      kernel/src/drivers/pci.c \
                    kernel/src/lib/string.c
bin/tests/acpi:     tests/test_acpi.c     kernel/src/cpu/acpi.c \
                    kernel/src/lib/string.c
bin/tests/auth:     tests/test_auth.c     kernel/src/sched/auth.c \
                    kernel/src/lib/string.c
bin/tests/tty:      tests/test_tty.c      kernel/src/drivers/tty.c \
                    kernel/src/sched/process.c kernel/src/lib/string.c
bin/tests/syscall:  tests/test_syscall.c  kernel/src/cpu/syscall.c \
                    kernel/src/sched/process.c kernel/src/lib/string.c \
                    kernel/src/fs/vfs.c
bin/tests/elf:      tests/test_elf.c      kernel/src/fs/elf.c \
                    kernel/src/lib/string.c
bin/tests/ramdisk:  tests/test_ramdisk.c  kernel/src/fs/ramdisk.c \
                    kernel/src/lib/string.c
bin/tests/fat32:    tests/test_fat32.c    kernel/src/fs/fat32.c \
                    kernel/src/lib/string.c
bin/tests/vfs:      tests/test_vfs.c      kernel/src/fs/vfs.c \
                    kernel/src/fs/ramdisk.c kernel/src/lib/string.c
bin/tests/philemon:  tests/test_philemon.c boot/philemon.c boot/philemon.h
bin/tests/gdt:      tests/test_gdt.c      kernel/src/cpu/gdt.c
bin/tests/gdt:      SRCS = tests/test_gdt.c
bin/tests/keyboard: tests/test_keyboard.c kernel/src/drivers/keyboard.c \
                    kernel/src/drivers/input.c
bin/tests/serial:   tests/test_serial.c   kernel/src/drivers/serial.c \
                    kernel/src/drivers/input.c
bin/tests/shell:    tests/test_shell.c    kernel/src/lib/string.c \
                    kernel/src/fs/ramdisk.c kernel/src/sched/auth.c \
                    kernel/src/drivers/pci.c kernel/src/fs/vfs.c \
                    kernel/src/shell/shell.c kernel/src/version.h
bin/tests/shell:    SRCS = tests/test_shell.c kernel/src/lib/string.c \
                           kernel/src/fs/ramdisk.c kernel/src/sched/auth.c \
                           kernel/src/drivers/pci.c kernel/src/fs/vfs.c

# SRCS overrides what gets compiled, for tests that #include a kernel
# .c file directly -- that file still belongs in the prerequisites so
# make rebuilds when it changes, but compiling it twice would give me
# duplicate symbols
$(filter-out bin/tests/switch,$(TEST_BINS)):
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTFLAGS) $(if $(SRCS),$(SRCS),$^) -o $@

# the switch test calls into the real switch.asm, and needs -no-pie so
# the `callq switch_context` in its inline asm resolves
obj/tests/switch.asm.o: kernel/src/sched/switch.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

bin/tests/switch: tests/test_switch.c obj/tests/switch.asm.o
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTFLAGS) -no-pie $^ -o $@

.PHONY: test
# the fat32 suite runs against a real filesystem rather than a fixture,
# so it gets a throwaway image built by the same tool that builds the
# real one. rebuilt every time, without fail: the tests write to it, and
# a suite that passes only on a disk its last run left behind is worse
# than no suite at all
.PHONY: fat32-image
fat32-image:
	@mkdir -p bin/tests
	@rm -f bin/tests/fat32.img
	@python3 tools/mkfat.py bin/tests/fat32.img diskroot 64 >/dev/null

test: checkfmt $(USER_PROGS) $(RAMDISK) $(TEST_BINS) fat32-image
	@fail=0; \
	for t in $(TEST_BINS); do \
		printf '  %-10s ' "$$(basename $$t)"; \
		case $$t in *fat32) arg=bin/tests/fat32.img;; *) arg=;; esac; \
		if out=$$(./$$t $$arg 2>&1); then \
			echo 'ok'; \
		else \
			echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
		fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo '  all suites passed'; else exit 1; fi

# gcc checks my format strings against real printf, which accepts far
# more than my kprintf implements. this catches the difference.
.PHONY: checkfmt
checkfmt:
	@printf '  %-10s ' checkfmt
	@python3 tools/checkfmt.py kernel/src && echo 'ok'

# boot the iso and drive the shell over serial. needs qemu
.PHONY: boottest
boottest: iso
	./tools/boottest.sh $(ISO)

clean:
	rm -rf bin obj iso_root $(ISO)

distclean: clean
