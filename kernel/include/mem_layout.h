/*
 * mem_layout.h — Physical memory layout for QEMU "virt" machine.
 *
 * Platform-specific constants: device addresses and DRAM boundaries.
 * These come from QEMU's hw/riscv/virt.c and the device tree.
 * A different board would have a different mem_layout.h.
 *
 * See Lecture 3-1 for the full address map and rationale.
 */

#ifndef MEM_LAYOUT_H
#define MEM_LAYOUT_H

/*
 * TODO: Define KERNBASE — the physical address where DRAM begins
 *       on the QEMU virt machine. The linker script places the
 *       kernel at this address. Use a UL suffix.
 *       (Hint: see Lecture 3-1, Part 1 address map)
 */

/*
 * TODO: Define PHYSTOP — the physical address where DRAM ends.
 *       QEMU virt provides 128 MB of RAM by default.
 *       Express it in terms of KERNBASE.
 */

/*
 * TODO: Define UART0_BASE — the base address of the 16550 UART.
 *       This was previously hardcoded in uart.c as UART0.
 *       Use a UL suffix.
 */

#endif /* MEM_LAYOUT_H */
