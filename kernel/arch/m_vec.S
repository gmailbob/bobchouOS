# m_vec.S — M-mode trap vector (mini-SBI) for bobchouOS.
#
# After the SSTC migration the timer lives in S-mode, so the only trap
# left for M-mode is the S-mode shutdown ecall (a7 = SBI_SHUTDOWN): write
# the QEMU test-finisher device, which halts the machine. Anything else
# is unexpected and parks the hart.
#
# Nothing here returns to S-mode, so we save/restore no registers and
# clobber freely — no mscratch save area needed.
#
# See Lecture 5-1, Part 4; lectures/stretch-sstc-timer for the timer removal.

#include "riscv.h"
#include "mem_layout.h"
#include "sbi.h"

.align 4
.globl m_vec

m_vec:
    # Dispatch on mcause. Anything that isn't an S-mode ecall is unexpected.
    csrr  t0, mcause
    bltz  t0, park              # bit 63 set → interrupt; none expected
    andi  t0, t0, 0xff          # exception cause code
    li    t1, EXC_ECALL_S
    bne   t0, t1, park          # not an ecall → unexpected

    # S-mode ecall: the only supported function is shutdown.
    li    t0, SBI_SHUTDOWN
    bne   a7, t0, park          # unknown SBI function

    # Shutdown: write the QEMU test finisher (32-bit write required).
    li    t0, QEMU_SHUTDOWN
    li    t1, QEMU_SHUTDOWN_PASS
    sw    t1, 0(t0)             # Machine halts on the write
    j     park                  # Falls through to park if it somehow doesn't
