/*
 * riscv.h — RISC-V constants, CSR access helpers, and bit definitions.
 *
 * This header is shared between C and assembly (.S) files.
 * Structure:
 *   1. Assembly-safe #defines (constants, bit masks) — usable from both
 *   2. C-only section (types, inline asm macros, inline functions)
 *
 * .S files are preprocessed by gcc, so #define works, but the
 * assembler does not understand C type suffixes (UL, ULL).
 * _UL(x) expands to xUL in C and plain x in assembly.
 * __ASSEMBLER__ is predefined by gcc when preprocessing .S files.
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

/* ===================================================================
 * Assembly-safe constants (usable from both .S and .c files)
 * =================================================================== */

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
#define SIP_STIP            (_UL(1) << 5)   /* supervisor timer pending (read-only under SSTC) */

/* ---- menvcfg (Machine Environment Configuration) bits ---- */
#define MENVCFG_STCE        (_UL(1) << 63)  /* gate: S-mode may write stimecmp */

/* ---- medeleg bits ---- */
#define MEDELEG_ECALL_S     (_UL(1) << 9)

/* ---- Interrupt cause codes (bit 63 = 1) ---- */
#define SCAUSE_INTERRUPT    (_UL(1) << 63)
#define IRQ_S_SOFT          1               /* supervisor software (SSIP) */
#define IRQ_S_TIMER         5               /* supervisor timer */
#define IRQ_S_EXT           9               /* supervisor external (PLIC) */
#define IRQ_M_TIMER         7               /* machine timer */

/* ---- Exception cause codes (bit 63 = 0, same for mcause and scause) ---- */
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
#define PG_SIZE             (_UL(1) << 12)  /* 4096 bytes per page */
#define PG_SHIFT            12              /* log2(PG_SIZE) */

#define PG_ROUND_UP(a)      (((a) + PG_SIZE - 1) & ~(PG_SIZE - 1))
#define PG_ROUND_DOWN(a)    ((a) & ~(PG_SIZE - 1))

/* ---- PMP (Physical Memory Protection) ---- */
#define PMP_NAPOT_ALL       _UL(0x3fffffffffffff)  /* all 54 addr bits set */
#define PMPCFG_TOR_RWX      _UL(0x0f)              /* TOR mode, R+W+X */

/* ---- CLINT (Core Local Interruptor) — QEMU virt MMIO addresses ---- */
#define CLINT_BASE          _UL(0x2000000)
/* mtimecmp/mtime macros retired by SSTC (stimecmp CSR + time CSR).
 * CLINT_BASE kept — Phase 9 uses msip[hart] for IPIs. */

/* ---- mcounteren (Machine Counter-Enable) bits ---- */
#define MCOUNTEREN_TM       (_UL(1) << 1)  /* gate: S-mode may read the `time` CSR (rdtime) */

/* ---- Timer ---- */
#define MTIME_FREQ          _UL(10000000)  /* QEMU virt: time runs at 10 MHz */
#define TIMER_INTERVAL      _UL(100000)    /* 100,000 ticks = 10ms at 10 MHz */
#define MS_TO_MTIME(ms)     ((ms) * (MTIME_FREQ / 1000))

// clang-format on

/* ===================================================================
 * C-only: types, inline asm macros, and inline functions.
 * The assembler cannot parse anything below this guard.
 * =================================================================== */

#ifndef __ASSEMBLER__

#include "types.h"

/* ---- Generic CSR read/write ---- */
#define csrr(csr)                                                                                  \
    ({                                                                                             \
        uint64 __val;                                                                              \
        asm volatile("csrr %0, " #csr : "=r"(__val));                                              \
        __val;                                                                                     \
    })

#define csrw(csr, val) ({ asm volatile("csrw " #csr ", %0" : : "r"(val)); })

/* ---- TLB flush ---- */
static inline void
sfence_vma(void) {
    asm volatile("sfence.vma zero, zero");
}

/* ---- satp helpers (Sv39) ---- */
#define SATP_SV39 (8UL << 60)
#define MAKE_SATP(root_pt) (SATP_SV39 | ((uint64)(root_pt) >> 12))

/* Current time via the `time` CSR. Requires mcounteren.TM (set in entry.S). */
static inline uint64
read_time(void) {
    return csrr(time);
}

/* ---- S-mode interrupt enable/disable ---- */
static inline unsigned long
intr_get(void) {
    return csrr(sstatus) & SSTATUS_SIE;
}

static inline void
intr_on(void) {
    csrw(sstatus, csrr(sstatus) | SSTATUS_SIE);
}

static inline void
intr_off(void) {
    csrw(sstatus, csrr(sstatus) & ~SSTATUS_SIE);
}

#endif /* !__ASSEMBLER__ */

#endif /* RISCV_H */
