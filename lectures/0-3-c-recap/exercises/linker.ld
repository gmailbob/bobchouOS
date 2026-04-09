/* linker.ld — Same layout as Lecture 0-1 bare-metal hello */

ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(16);
    . += 4096;
    stack_top = .;
}
