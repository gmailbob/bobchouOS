# entry.S — M-mode boot code for bobchouOS (runs once).
#
# One-time initialization in M-mode, then mret into S-mode (kmain).
# The runtime M-mode trap handler lives in m_vec.S (separate file).
#
# Boot steps:
#   1. Park non-boot harts
#   2. Zero BSS
#   3. Set up boot stack (becomes scheduler stack later)
#   4. Set mstatus.MPP = Supervisor (for mret)
#   5. Set mepc = kmain (jump target after mret)
#   6. Disable paging (satp = 0)
#   7. Delegate traps to S-mode
#   8. Configure PMP (allow S-mode full physical access)
#   9. Set up M-mode trap vector (m_vec, shutdown ecall only)
#  10. Open S-mode timer gates: menvcfg.STCE (stimecmp) + mcounteren.TM (time)
#  11. Store hart ID in tp (for this_cpu())
#  12. mret → S-mode → kmain()
#
# Refer to Lectures 1-1, 2-1, 2-3, and 5-1.

#include "riscv.h"

.section .text.entry
.global _start
.global park

_start:
    # ---- Step 1: Hart parking ----
    csrr  t0, mhartid
    bnez  t0, park

    # ---- Step 2: Zero BSS ----
    la    t0, _bss_start
    la    t1, _bss_end
bss_clear:
    bgeu  t0, t1, bss_done
    sd    zero, 0(t0)
    addi  t0, t0, 8
    j     bss_clear
bss_done:

    # ---- Step 3: Set up stack ----
    la    sp, stack_top

    # ---- Step 4: Set mstatus.MPP to Supervisor ----
    csrr  t0, mstatus
    li    t1, ~MSTATUS_MPP_MASK
    and   t0, t0, t1              # clear MPP field
    li    t1, MSTATUS_MPP_S
    or    t0, t0, t1              # MPP = 01 (Supervisor)
    csrw  mstatus, t0             # mret will switch to S-mode

    # ---- Step 5: Set mepc to kmain ----
    la    t0, kmain
    csrw  mepc, t0                # mret jumps here

    # ---- Step 6: Disable paging ----
    csrw  satp, zero              # bare mode until vm_enable_paging()

    # ---- Step 7: Delegate traps to S-mode ----
    li    t0, 0xffff
    csrw  mideleg, t0             # S-mode handles its own interrupts
    andi  t0, t0, ~MEDELEG_ECALL_S # clear bit 9; ~(1<<9) = -513, fits in 12-bit signed imm
    csrw  medeleg, t0             # S-mode handles its own exceptions, except ecall

    # ---- Step 8: Configure PMP ----
    li    t0, PMP_NAPOT_ALL
    csrw  pmpaddr0, t0            # cover entire address space
    li    t0, PMPCFG_TOR_RWX
    csrw  pmpcfg0, t0             # grant S-mode full R/W/X access

    # ---- Step 9: Set up M-mode trap vector ----
    # m_vec.S now handles only the S-mode shutdown ecall. It never
    # returns, so it needs no mscratch save area.
    la    t0, m_vec
    csrw  mtvec, t0               # M-mode traps go to m_vec

    # ---- Step 10: Open the S-mode timer gates (SSTC) ----
    li    t0, 1
    slli  t0, t0, 63              # MENVCFG_STCE (bit 63 — too wide for an li immediate)
    csrw  menvcfg, t0             # menvcfg.STCE: S-mode may write stimecmp
    li    t0, MCOUNTEREN_TM
    csrw  mcounteren, t0          # mcounteren.TM: S-mode may read the time CSR

    # ---- Step 11: Store hart ID in tp ----
    # tp is ABI-reserved (never modified by any function).
    # this_cpu() reads tp to index cpus[]. With one hart, tp = 0.
    csrr  tp, mhartid

    # ---- Step 12: Enter S-mode ----
    mret

park:
    wfi
    j     park
