# trapvec.S — S-mode trap vector for bobchouOS
#
# When any S-mode trap occurs (interrupt, exception, ecall), the CPU
# jumps here (via stvec).  This code saves the caller-saved registers
# onto the kernel stack, calls the C handler kerneltrap(), restores
# the registers, and returns via sret.
#
# We only save caller-saved registers (ra, t0-t6, a0-a7) because the
# C calling convention guarantees kerneltrap() preserves callee-saved
# registers (s0-s11, sp, gp, tp).
#
# Refer to Lecture 2-2, Parts 5-6.

.section .text
.globl kernelvec
.align 4

kernelvec:
    # ---- Save caller-saved registers ----
    addi  sp, sp, -128

    sd    x1,    0(sp)    # ra
    sd    x5,    8(sp)    # t0
    sd    x6,   16(sp)    # t1
    sd    x7,   24(sp)    # t2
    sd    x10,  32(sp)    # a0
    sd    x11,  40(sp)    # a1
    sd    x12,  48(sp)    # a2
    sd    x13,  56(sp)    # a3
    sd    x14,  64(sp)    # a4
    sd    x15,  72(sp)    # a5
    sd    x16,  80(sp)    # a6
    sd    x17,  88(sp)    # a7
    sd    x28,  96(sp)    # t3
    sd    x29, 104(sp)    # t4
    sd    x30, 112(sp)    # t5
    sd    x31, 120(sp)    # t6

    # ---- Call C trap handler ----
    call kerneltrap

    # ---- Restore caller-saved registers ----
    ld    x1,    0(sp)    # ra
    ld    x5,    8(sp)    # t0
    ld    x6,   16(sp)    # t1
    ld    x7,   24(sp)    # t2
    ld    x10,  32(sp)    # a0
    ld    x11,  40(sp)    # a1
    ld    x12,  48(sp)    # a2
    ld    x13,  56(sp)    # a3
    ld    x14,  64(sp)    # a4
    ld    x15,  72(sp)    # a5
    ld    x16,  80(sp)    # a6
    ld    x17,  88(sp)    # a7
    ld    x28,  96(sp)    # t3
    ld    x29, 104(sp)    # t4
    ld    x30, 112(sp)    # t5
    ld    x31, 120(sp)    # t6

    addi  sp, sp, 128

    # ---- Return from trap ----
    sret
