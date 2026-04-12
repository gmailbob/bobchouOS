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
    # TODO: Allocate space on the stack for 16 registers x 8 bytes.
    #       Save ra, t0-t2, a0-a7, t3-t6 onto the stack using sd.
    #       (Refer to Lecture 2-2, Part 6 for the layout.)

    # ---- Call C trap handler ----
    # TODO: Call kerneltrap (defined in trap.c).

    # ---- Restore caller-saved registers ----
    # TODO: Restore all the registers you saved above using ld.
    #       Reclaim the stack space.

    # ---- Return from trap ----
    # TODO: Execute sret to return to the interrupted code.
