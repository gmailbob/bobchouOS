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

/*
 * User-mode virtual address space layout (Sv39, high → low):
 *   TRAMPOLINE      — shared kernel/user page (uservec/userret)
 *   TRAPFRAME       — per-process saved registers
 *   (guard page)    — unmapped, separates trapframe from stack
 *   USER_STACK_TOP  — top of stack region
 *   USER_STACK_BOT  — bottom of stack region (elastic, up to 16 pages)
 *   (guard page)    — unmapped, separates stack from heap
 *   HEAP_MAX        — highest address sbrk can reach
 *       ...         — heap grows upward from end of PT_LOAD segments
 *   USER_TEXT_START  — first mapped page (page 0 intentionally unmapped)
 */
#define TRAMPOLINE       (MAX_VA - PG_SIZE)
#define TRAPFRAME        (TRAMPOLINE - PG_SIZE)
#define USER_STACK_TOP   (TRAPFRAME - PG_SIZE)
#define STACK_MAX_PAGES  16
#define USER_STACK_BOT   (USER_STACK_TOP - STACK_MAX_PAGES * PG_SIZE)
#define HEAP_MAX         (USER_STACK_BOT - PG_SIZE)
#define USER_TEXT_START  (_UL(0x1000))
// clang-format on

#endif /* MEM_LAYOUT_H */
