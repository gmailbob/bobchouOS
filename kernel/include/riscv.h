/*
 * riscv.h — RISC-V CSR access helpers and bit definitions.
 *
 * Provides generic macros for reading/writing any CSR, plus
 * named constants for commonly used bit fields.
 *
 * This header is shared between C and assembly (.S) files.
 * .S files are preprocessed by gcc, so #define works, but the
 * assembler does not understand C type suffixes (UL, ULL).
 * _UL(x) expands to xUL in C and plain x in assembly.
 * __ASSEMBLER__ is predefined by gcc when preprocessing .S files,
 * so the same header produces different output depending on context.
 *
 * Refer to Lecture 2-1, Part 9 for a detailed walkthrough.
 */

#ifndef RISCV_H
#define RISCV_H
#ifdef __ASSEMBLER__
#define _UL(x) x
#else
#define _UL(x) x##UL
#endif

/* C-only: types and inline asm macros that the assembler can't parse. */
#ifndef __ASSEMBLER__

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

#endif /* !__ASSEMBLER__ */

// clang-format off
/* ---- mstatus bits ---- */
#define MSTATUS_MPP_MASK    (_UL(3) << 11)
#define MSTATUS_MPP_M       (_UL(3) << 11)
#define MSTATUS_MPP_S       (_UL(1) << 11)
#define MSTATUS_MPP_U       (_UL(0) << 11)
#define MSTATUS_MIE         (_UL(1) << 3)
#define MSTATUS_MPIE        (_UL(1) << 7)

/* ---- sstatus bits ---- */
#define SSTATUS_SPP         (_UL(1) << 8)
#define SSTATUS_SPIE        (_UL(1) << 5)
#define SSTATUS_SIE         (_UL(1) << 1)

/* ---- mie (Machine Interrupt Enable) bits ---- */
#define MIE_MTIE            (_UL(1) << 7)   /* machine timer */

/* ---- sie (Supervisor Interrupt Enable) bits ---- */
#define SIE_SSIE            (_UL(1) << 1)   /* supervisor software */
#define SIE_STIE            (_UL(1) << 5)   /* supervisor timer */
#define SIE_SEIE            (_UL(1) << 9)   /* supervisor external */

/* ---- sip (Supervisor Interrupt Pending) bits ---- */
#define SIP_SSIP            (_UL(1) << 1)   /* supervisor software pending */

/* ---- scause ---- */
#define SCAUSE_INTERRUPT    (_UL(1) << 63)

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

/* ---- Page size and alignment (Sv39) ---- */

/*
 * TODO: Define PGSIZE — the number of bytes in one page.
 *       This is determined by the Sv39 page table format.
 *       (Hint: 2^12)
 */

/*
 * TODO: Define PGSHIFT — the number of bits in the page offset.
 *       This is log2(PGSIZE).
 */

/*
 * TODO: Define PGROUNDUP(a) — round address a UP to the next
 *       page boundary. If already aligned, return unchanged.
 *       (Hint: add PGSIZE-1, then clear the low bits)
 */

/*
 * TODO: Define PGROUNDDOWN(a) — round address a DOWN to the
 *       nearest page boundary.
 *       (Hint: clear the low PGSHIFT bits)
 */

/* ---- PMP (Physical Memory Protection) ---- */
#define PMP_NAPOT_ALL       _UL(0x3fffffffffffff)  /* all 54 addr bits set */
#define PMPCFG_TOR_RWX      _UL(0x0f)              /* TOR mode, R+W+X */

/* ---- CLINT memory-mapped registers (QEMU virt) ---- */
#define CLINT_BASE          _UL(0x2000000)
#define CLINT_MTIMECMP(hart) (CLINT_BASE + 0x4000 + 8 * (hart))
#define CLINT_MTIME         (CLINT_BASE + 0xBFF8)

/* Timer interval: 100,000 ticks = 10ms at 10 MHz */
#define TIMER_INTERVAL      _UL(100000)
// clang-format on

#endif /* RISCV_H */
