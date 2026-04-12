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
// clang-format on

#endif /* RISCV_H */
