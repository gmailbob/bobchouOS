/*
 * linker.ld — Kernel linker script for bobchouOS on QEMU "virt" machine.
 *
 * Memory layout:
 *
 *   0x80000000  ┌──────────────┐ ← _kernel_start
 *               │  .text       │  Code (entry.S first)
 *               ├──────────────┤ ← _text_end (page-aligned)
 *               │  .rodata     │  Read-only data
 *               ├──────────────┤
 *               │  .data       │  Initialized globals
 *               ├──────────────┤ ← _bss_start
 *               │  .bss        │  Uninitialized globals (zeroed by entry.S)
 *               ├──────────────┤ ← _bss_end
 *               │  16KB stack  │  Stack grows downward
 *               ├──────────────┤ ← stack_top (sp starts here)
 *               └──────────────┘ ← _kernel_end
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    _kernel_start = .;

    /* Code section. .text.entry comes first so _start is at 0x80000000. */
    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    . = ALIGN(4096);
    _text_end = .;

    /* Read-only data (string literals, const arrays). */
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(16);

    /* Initialized global variables. */
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);

    /* Uninitialized globals — zeroed by entry.S before calling C. */
    _bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    _bss_end = .;

    /* Kernel stack: 16KB, 16-byte aligned. */
    . = ALIGN(16);
    . += 0x4000;
    stack_top = .;

    _kernel_end = .;
}
