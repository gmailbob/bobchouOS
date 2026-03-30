/*
 * linker.ld — Linker script for bare-metal RISC-V on QEMU "virt" machine.
 *
 * A linker script answers two questions:
 *   1. Where in memory does each piece of the program go?
 *   2. What is the entry point (first instruction to execute)?
 *
 * QEMU's "virt" machine loads the kernel at 0x80000000 and jumps there.
 * So we must place our entry code (_start from entry.S) at exactly that
 * address.
 *
 * Memory layout we're creating:
 *
 *   0x80000000  ┌──────────────┐
 *               │  .text       │  Code (entry.S first, then main.c)
 *               ├──────────────┤
 *               │  .rodata     │  Read-only data (string literals)
 *               ├──────────────┤
 *               │  .data       │  Initialized global variables
 *               ├──────────────┤
 *               │  .bss        │  Uninitialized globals (zeroed)
 *               ├──────────────┤
 *               │  (4KB stack) │  Stack space for our C code
 *               ├──────────────┤
 *               │  stack_top   │  Stack pointer starts here (grows down)
 *               └──────────────┘
 */

/* Tell the linker that _start is the entry point. */
ENTRY(_start)

SECTIONS
{
    /* Start placing sections at 0x80000000. */
    . = 0x80000000;

    /* Code section. .text.entry comes first so _start is at the very
       beginning — the address QEMU jumps to. */
    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    /* Read-only data (our "Hello from bobchouOS!\n" string lands here). */
    .rodata : {
        *(.rodata .rodata.*)
    }

    /* Initialized data (we have none in this example, but included for
       completeness). */
    .data : {
        *(.data .data.*)
    }

    /* Uninitialized data, zeroed at startup. The ALIGN(16) ensures proper
       alignment. */
    .bss : {
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    }

    /* Stack: 4KB is more than enough for this tiny program.
       The stack grows downward, so sp will be set to stack_top. */
    . = ALIGN(16);
    . += 4096;
    stack_top = .;
}
