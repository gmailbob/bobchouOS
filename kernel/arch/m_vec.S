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
    csrrw a0, mscratch, a0     # a0 = scratch area, mscratch = old a0
    sd    t0, 0(a0)
    sd    t1, 8(a0)

    # Read mcause to determine trap type.
    # Check if it's an interrupt (bit 63) or exception.
    # If interrupt: check if code == 7 (machine timer) → jump to m_timer
    # If exception: check if code == 9 (ecall from S) → jump to m_ecall
    # Otherwise: spin (unexpected trap)
    csrr  t0, mcause
    andi  t1, t0, 0xff
    bgez  t0, m_except         # branch if >= 0 i.e. bit 63 clear
    li    t0, IRQ_M_TIMER
    beq   t0, t1, m_timer
    j     park

m_except:
    li    t0, EXC_ECALL_S
    beq   t0, t1, m_ecall
    j     park
    

m_timer:
    # Raise supervisor software interrupt to notify S-mode.
    li    t0, SIP_SSIP
    csrs  sip, t0
    # set &mtimecmp[0] to max
    li    t0, CLINT_MTIMECMP(0)
    li    t1, -1
    sd    t1, 0(t0)

    ld    t0, 0(a0)
    ld    t1, 8(a0)
    csrrw a0, mscratch, a0
    mret

m_ecall:
    li    t0, SBI_SET_TIMER
    beq   t0, a7, set_timer
    li    t0, SBI_SHUTDOWN
    beq   t0, a7, shutdown
    j     park

set_timer:
    # Advance mepc past the ecall instruction
    csrr  t0, mepc
    addi  t0, t0, 4
    csrw  mepc, t0

    csrr  t0, mscratch    # original a0 that holds the deadline value
    li    t1, CLINT_MTIMECMP(0)
    sd    t0, 0(t1)

    ld    t0, 0(a0)
    ld    t1, 8(a0)
    csrrw a0, mscratch, a0
    mret

shutdown:
    li    t0, QEMU_SHUTDOWN
    li    t1, QEMU_SHUTDOWN_PASS
    sw    t1, 0(t0)            # 4-byte write — QEMU test finisher expects 32-bit
    # halt, no need to set register values back

# ---------- Scratch area for m_vec (BSS) ----------

.section .bss
.align 4
.globl m_scratch

m_scratch:
    .zero 16          # 2 x 8 bytes (register save slots)
