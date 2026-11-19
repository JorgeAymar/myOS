/* Static ring 3 programs: loaded at 4 MiB in their own address space. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);    /* R X */
    data PT_LOAD FLAGS(6);    /* R W */
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
