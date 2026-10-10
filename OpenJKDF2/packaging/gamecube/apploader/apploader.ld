OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(apploaderEntry)

SECTIONS
{
    . = 0x81200000;
    .text   : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) *(.sdata .sdata.*) }
    . = ALIGN(32);
    __image_end = .;
    .bss (NOLOAD) : { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.gnu.attributes) }
}
