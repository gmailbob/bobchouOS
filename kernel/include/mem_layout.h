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

#define QEMU_SHUTDOWN      (_UL(0x100000))        /* QEMU virt "test finisher" MMIO device */
#define QEMU_SHUTDOWN_PASS (_UL(0x5555))          /* exit QEMU with success */
#define QEMU_SHUTDOWN_FAIL (_UL(0x3333))          /* exit QEMU with failure */

/* User-mode virtual address space layout (Sv39: 2^39 = 512 GB) */
#define TRAMPOLINE       (MAX_VA - PG_SIZE)       /* trampoline page (same VA in both PTs) */
#define TRAPFRAME        (TRAMPOLINE - PG_SIZE)   /* per-process trapframe (user PT only) */
#define USER_STACK_TOP   (TRAPFRAME - PG_SIZE)    /* guard page between trapframe and stack */
#define USER_TEXT_START  (_UL(0x1000))            /* user code starts here (page 0 unmapped) */
// clang-format on

#endif /* MEM_LAYOUT_H */
