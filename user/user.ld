/*
 * user/user.ld — Linker script for user programs.
 *
 * User code starts at VA 0x1000 (page 0 is unmapped for NULL-deref protection).
 * See Lecture 6-1, Part 8 and Lecture 6-3, Part 4.
 */

ENTRY(_start)

SECTIONS {
    . = 0x1000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
}
