# m_vec.S — M-mode trap vector for bobchouOS.
#
# Handles two cases:
#   1. Machine timer interrupt (mcause = 7):
#      Set SSIP to forward to S-mode. Do NOT reschedule the next tick —
#      S-mode will arm the timer via ecall when the scheduler picks the
#      next process (on-demand one-shot timer).
#
#   2. Ecall from S-mode (mcause = 9):
#      Dispatch on a7:
#        SBI_SET_TIMER (0) — write a0 to mtimecmp[0]
#        SBI_SHUTDOWN  (1) — write to QEMU shutdown device
#      Advance mepc += 4 (skip the ecall instruction), then mret.
#
# Register save strategy: use mscratch to swap a0, then use a0 as a
# base for a small scratch area. Save only the registers we clobber.
#
# See Lecture 5-1, Part 4.

#include "riscv.h"
#include "mem_layout.h"
#include "sbi.h"

.align 4
.globl m_vec

m_vec:
    # TODO: Read mcause to determine trap type.
    #   csrr t0, mcause
    #
    # TODO: Check if it's an interrupt (bit 63) or exception.
    #   If interrupt: check if code == 7 (machine timer) → jump to m_timer
    #   If exception: check if code == 9 (ecall from S) → jump to m_ecall
    #   Otherwise: spin (unexpected trap)

    j m_vec   # placeholder — replace with dispatch logic

m_timer:
    # TODO: Raise supervisor software interrupt to notify S-mode.
    #   li  t0, SIP_SSIP
    #   csrs sip, t0
    #
    # Note: do NOT update mtimecmp here. The scheduler will arm the
    # next timer via SBI_SET_TIMER ecall when it picks the next process.
    #
    # TODO: mret

    mret      # placeholder

m_ecall:
    # TODO: Save registers we need to clobber (use mscratch scratch area).
    #
    # TODO: Read a7 to determine which SBI function was requested.
    #
    # TODO: If a7 == SBI_SET_TIMER:
    #   Write a0 to CLINT_MTIMECMP(0).
    #   (Load address of mtimecmp, store a0 there.)
    #
    # TODO: If a7 == SBI_SHUTDOWN:
    #   Write QEMU_SHUTDOWN_PASS to QEMU_SHUTDOWN address.
    #   (The machine will halt — no need to mret.)
    #
    # TODO: Advance mepc past the ecall instruction:
    #   csrr t0, mepc
    #   addi t0, t0, 4
    #   csrw mepc, t0
    #
    # TODO: Restore clobbered registers.
    #
    # TODO: mret

    mret      # placeholder

# ---------- Scratch area for m_vec (BSS) ----------

.section .bss
.align 4
.globl m_scratch

m_scratch:
    .zero 40          # 5 x 8 bytes (register save slots)
