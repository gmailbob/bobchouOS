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

#include "riscv.h"

// clang-format off
#define KERN_BASE    (_UL(0x80000000))            /* start of DRAM */
#define PHYS_STOP    (_UL(0x88000000))            /* end of DRAM (128 MB) */

#define UART0_BASE   (_UL(0x10000000))            /* 16550 UART */
#define PLIC_BASE    (_UL(0x0C000000))            /* Platform-Level Interrupt Controller */
#define PLIC_SIZE    (_UL(0x400000))              /* 4 MB — matches xv6 */
// clang-format on

#endif /* MEM_LAYOUT_H */
