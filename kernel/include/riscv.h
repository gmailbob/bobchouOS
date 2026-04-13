/*
 * riscv.h — RISC-V CSR access helpers and bit definitions.
 *
 * Provides generic macros for reading/writing any CSR, plus
 * named constants for commonly used bit fields.
 *
 * Refer to Lecture 2-1, Part 9 for a detailed walkthrough.
 */

#ifndef RISCV_H
#define RISCV_H

#include "types.h"

/* ---- Generic CSR read/write macros ----
 *
 * Usage:
 *   uint64 val = csrr(sstatus);
 *   csrw(satp, 0);
 *
 * These use GCC statement expressions ({ ... }) so the read macro
 * can return a value.  The # operator stringifies the csr argument
 * so that csrr(sstatus) expands to the assembly string "sstatus".
 */
#define csrr(csr)                                                                                  \
    ({                                                                                             \
        uint64 __val;                                                                              \
        asm volatile("csrr %0, " #csr : "=r"(__val));                                              \
        __val;                                                                                     \
    })

#define csrw(csr, val) ({ asm volatile("csrw " #csr ", %0" : : "r"(val)); })

// clang-format off
/* ---- mstatus bits ---- */
#define MSTATUS_MPP_MASK    (3UL << 11)
#define MSTATUS_MPP_M       (3UL << 11)
#define MSTATUS_MPP_S       (1UL << 11)
#define MSTATUS_MPP_U       (0UL << 11)
#define MSTATUS_MIE         (1UL << 3)
#define MSTATUS_MPIE        (1UL << 7)

/* ---- sstatus bits ---- */
#define SSTATUS_SPP         (1UL << 8)
#define SSTATUS_SPIE        (1UL << 5)
#define SSTATUS_SIE         (1UL << 1)

/* ---- mie (Machine Interrupt Enable) bits ---- */
#define MIE_MTIE            (1UL << 7)   /* machine timer */

/* ---- sie (Supervisor Interrupt Enable) bits ---- */
#define SIE_SSIE            (1UL << 1)   /* supervisor software */
#define SIE_STIE            (1UL << 5)   /* supervisor timer */
#define SIE_SEIE            (1UL << 9)   /* supervisor external */

/* ---- sip (Supervisor Interrupt Pending) bits ---- */
#define SIP_SSIP            (1UL << 1)   /* supervisor software pending */

/* ---- CLINT memory-mapped registers (QEMU virt) ---- */
#define CLINT_BASE          0x2000000UL
#define CLINT_MTIMECMP(hart) (CLINT_BASE + 0x4000 + 8 * (hart))
#define CLINT_MTIME         (CLINT_BASE + 0xBFF8)

/* Timer interval: 100,000 ticks = 10ms at 10 MHz */
#define TIMER_INTERVAL      100000UL

/* ---- scause ---- */
#define SCAUSE_INTERRUPT    (1UL << 63)

/* Interrupt cause codes (scause value when bit 63 = 1) */
#define IRQ_S_SOFT          1
#define IRQ_S_TIMER         5
#define IRQ_S_EXT           9

/* Exception cause codes (scause value when bit 63 = 0) */
#define EXC_INST_MISALIGN   0
#define EXC_INST_ACCESS     1
#define EXC_ILLEGAL_INST    2
#define EXC_BREAKPOINT      3
#define EXC_LOAD_MISALIGN   4
#define EXC_LOAD_ACCESS     5
#define EXC_STORE_MISALIGN  6
#define EXC_STORE_ACCESS    7
#define EXC_ECALL_U         8
#define EXC_ECALL_S         9
#define EXC_INST_PAGE       12
#define EXC_LOAD_PAGE       13
#define EXC_STORE_PAGE      15
// clang-format on

#endif /* RISCV_H */
