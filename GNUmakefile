# tinyOS build. needs gcc, nasm, make. iso/run additionally need xorriso + qemu.
#
# targets:
#   make        -> kernel elf in bin/
#   make iso    -> bootable hybrid bios/uefi iso
#   make run    -> boot it in qemu
#   make clean

KERNEL := tinyos
ISO    := tinyos.iso

CC   := gcc
LD   := ld
NASM := nasm

# freestanding kernel flags. the -mno-* soup is because we cant use fpu/sse
# in the kernel (no context saving yet), and no red zone because interrupts
# would trash it
CFLAGS := -g -Wall -Wextra -std=gnu11 \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIC \
	-m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
	-mcmodel=kernel -Ikernel/src -MMD -MP -fno-omit-frame-pointer

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld

NASMFLAGS := -f elf64 -g

CSRC := $(shell find kernel/src -name '*.c')
ASRC := $(shell find kernel/src -name '*.asm')
OBJ  := $(patsubst kernel/src/%.c,obj/%.c.o,$(CSRC)) \
        $(patsubst kernel/src/%.asm,obj/%.asm.o,$(ASRC))

.PHONY: all iso run run-uefi clean distclean

all: bin/$(KERNEL)

# two passes, because the symbol table describes addresses and linking
# it in changes them. .ksyms sits after .text in the linker script, so
# folding it in shifts .data but cannot move a single function -- and
# gensyms --check proves that held instead of us just hoping.
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

-include $(OBJ:.o=.d)

# limine binary release, pinned to the v9.x branch. shallow clone bc we only
# want the prebuilt blobs + the host install tool
limine/limine:
	test -d limine || git clone --branch=v9.x-binary --depth=1 \
		https://github.com/limine-bootloader/limine.git limine
	$(MAKE) -C limine

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
              ramdisk/bin/whoami

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

iso: bin/$(KERNEL) $(RAMDISK) limine/limine
	rm -rf iso_root
	mkdir -p iso_root/boot/limine iso_root/EFI/BOOT
	cp bin/$(KERNEL) iso_root/boot/
	cp $(RAMDISK) iso_root/boot/
	cp limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO)
	./limine/limine bios-install $(ISO)
	rm -rf iso_root

run: iso
	qemu-system-x86_64 -M q35 -m 2G -cdrom $(ISO) -serial stdio

# needs edk2-ovmf installed (fedora path below)
run-uefi: iso
	qemu-system-x86_64 -M q35 -m 2G -cdrom $(ISO) -serial stdio \
		-drive if=pflash,unit=0,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd

# ---- host tests -----------------------------------------------------
#
# the testable guts of the kernel are deliberately split from the parts
# that touch hardware: pmm_init_from_map() takes a memory map instead of
# asking limine, keyboard_feed() takes a scancode instead of reading a
# port, and so on. that lets all of this run as ordinary linux programs.
# TINYOS_HOSTED turns irq_save/irq_restore into no-ops, since userspace
# gets shot for saying cli.

HOSTCC    := gcc
HOSTFLAGS := -std=gnu11 -Wall -Wextra -g -DTINYOS_HOSTED -Ikernel/src

TEST_BINS := bin/tests/kprintf bin/tests/mm bin/tests/vmm bin/tests/gdt \
             bin/tests/ksyms bin/tests/rtc bin/tests/ramdisk bin/tests/elf \
             bin/tests/addrspace bin/tests/process \
             bin/tests/syscall bin/tests/tty bin/tests/auth bin/tests/acpi bin/tests/pci \
             bin/tests/keyboard bin/tests/serial \
             bin/tests/shell bin/tests/switch

bin/tests/kprintf:  tests/test_kprintf.c  kernel/src/lib/kprintf.c
bin/tests/mm:       tests/test_mm.c       kernel/src/mm/pmm.c \
                    kernel/src/mm/kmalloc.c kernel/src/lib/string.c
bin/tests/addrspace: tests/test_addrspace.c kernel/src/mm/addrspace.c \
                    kernel/src/mm/vmm.c kernel/src/lib/string.c
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
                    kernel/src/sched/process.c kernel/src/lib/string.c
bin/tests/elf:      tests/test_elf.c      kernel/src/fs/elf.c \
                    kernel/src/lib/string.c
bin/tests/ramdisk:  tests/test_ramdisk.c  kernel/src/fs/ramdisk.c \
                    kernel/src/lib/string.c
bin/tests/gdt:      tests/test_gdt.c      kernel/src/cpu/gdt.c
bin/tests/gdt:      SRCS = tests/test_gdt.c
bin/tests/keyboard: tests/test_keyboard.c kernel/src/drivers/keyboard.c \
                    kernel/src/drivers/input.c
bin/tests/serial:   tests/test_serial.c   kernel/src/drivers/serial.c \
                    kernel/src/drivers/input.c
bin/tests/shell:    tests/test_shell.c    kernel/src/lib/string.c \
                    kernel/src/fs/ramdisk.c kernel/src/sched/auth.c \
                    kernel/src/drivers/pci.c \
                    kernel/src/shell/shell.c kernel/src/version.h
bin/tests/shell:    SRCS = tests/test_shell.c kernel/src/lib/string.c \
                           kernel/src/fs/ramdisk.c kernel/src/sched/auth.c \
                           kernel/src/drivers/pci.c

# SRCS overrides what gets compiled, for tests that #include a kernel
# .c file directly -- that file still belongs in the prerequisites so
# make rebuilds when it changes, but compiling it twice would give us
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
test: checkfmt $(USER_PROGS) $(RAMDISK) $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		printf '  %-10s ' "$$(basename $$t)"; \
		if out=$$(./$$t 2>&1); then \
			echo 'ok'; \
		else \
			echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
		fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo '  all suites passed'; else exit 1; fi

# gcc checks our format strings against real printf, which accepts far
# more than our kprintf implements. this catches the difference.
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
	rm -rf limine
