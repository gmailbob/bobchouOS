# m_vec.S — M-mode trap vector (mini-SBI) for bobchouOS.
#
# Handles one trap type (the SSTC timer no longer bounces through M-mode):
#   Ecall from S-mode (mcause bit63=0, code=9):
#     Dispatch on a7:
#       SBI_SHUTDOWN (1) — write to QEMU shutdown device
#     Advance mepc += 4 (skip ecall), then mret.
#
# Register strategy: swap a0 with mscratch (gives scratch area pointer),
# save t0/t1 to scratch area. Only these three registers are clobbered.
# All paths converge on m_ret for shared epilogue.
#
# See Lecture 5-1, Part 4; lectures/stretch-sstc-timer for the timer removal.

#include "riscv.h"
#include "mem_layout.h"
#include "sbi.h"

.align 4
.globl m_vec

# --- Entry: save clobbered registers ---
m_vec:
    csrrw a0, mscratch, a0      # a0 ↔ mscratch (a0 = scratch, mscratch = orig a0)
    sd    t0, 0(a0)
    sd    t1, 8(a0)

# --- Dispatch on mcause ---
    csrr  t0, mcause
    andi  t1, t0, 0xff          # t1 = cause code (low 8 bits)
    bgez  t0, m_except          # bit 63 clear → exception
    j     park                  # no M-mode interrupts expected

m_except:
    li    t0, EXC_ECALL_S
    beq   t1, t0, m_ecall
    j     park                  # unexpected exception

# --- Ecall: dispatch on a7 ---
m_ecall:
    li    t0, SBI_SHUTDOWN
    beq   a7, t0, m_shutdown
    j     park                 # unknown SBI function

m_shutdown:
    li    t0, QEMU_SHUTDOWN
    li    t1, QEMU_SHUTDOWN_PASS
    sw    t1, 0(t0)            # 32-bit write (QEMU test finisher requirement)
    j     m_ret                # unreachable — machine halts

# --- Shared epilogue: restore and return ---
m_ret:
    ld    t0, 0(a0)
    ld    t1, 8(a0)
    csrrw a0, mscratch, a0     # restore a0
    mret

# --- Scratch area (BSS) ---
.section .bss
.align 4
.globl m_scratch

m_scratch:
    .zero 16                   # 2 × 8 bytes: save slots for t0, t1
