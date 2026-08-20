/* linker script for the kernel. mostly lifted from what the limine template
 * does, since getting this wrong means the bootloader just wont find the
 * requests section and you boot into nothing.
 *
 * the __*_start/__*_end symbols exist so the vmm can map each section with
 * the permissions it deserves: text executable but not writable, rodata
 * neither, data writable but never executable. every section boundary is
 * page aligned because permissions come at page granularity. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    /* higher half, top 2GiB. this is what -mcmodel=kernel expects */
    . = 0xffffffff80000000;

    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_end = .;
    __rodata_start = .;

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_end = .;
    __data_start = .;

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_end = .;
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
