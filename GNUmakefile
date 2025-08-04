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
	-mcmodel=kernel -Ikernel/src -MMD -MP

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld

NASMFLAGS := -f elf64 -g

# make FAULT_DEMO=1 -> kernel pokes a bad pointer at boot to show off the
# exception handler. remember to `make clean` first when toggling this,
# the makefile isnt smart enough to notice the flag changed
ifeq ($(FAULT_DEMO),1)
CFLAGS += -DFAULT_DEMO
endif

CSRC := $(shell find kernel/src -name '*.c')
ASRC := $(shell find kernel/src -name '*.asm')
OBJ  := $(patsubst kernel/src/%.c,obj/%.c.o,$(CSRC)) \
        $(patsubst kernel/src/%.asm,obj/%.asm.o,$(ASRC))

.PHONY: all iso run run-uefi clean distclean

all: bin/$(KERNEL)

bin/$(KERNEL): $(OBJ) kernel/linker.ld
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) $(OBJ) -o $@

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

iso: bin/$(KERNEL) limine/limine
	rm -rf iso_root
	mkdir -p iso_root/boot/limine iso_root/EFI/BOOT
	cp bin/$(KERNEL) iso_root/boot/
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

clean:
	rm -rf bin obj iso_root $(ISO)

distclean: clean
	rm -rf limine
