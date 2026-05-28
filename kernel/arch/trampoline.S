# trampoline.S — User-mode trap entry and return.
#
# This page is mapped at the same VA (TRAMPOLINE) in both kernel and
# user page tables, allowing safe page table switching mid-execution.
#
# Contains:
#   user_vec — entry point for traps from user mode (via stvec)
#   user_ret — return to user mode (called from user_trap_ret)
#
# Both use a0 as the trapframe base pointer. Offsets must match
# struct trapframe in trapframe.h.
#
# See Lecture 6-1, Part 6.

#include "riscv.h"

.section .trampoline
.globl user_vec
.globl user_ret
.align 4

user_vec:
    # State on entry:
    #   - CPU is in S-mode (hardware switched on trap)
    #   - satp still points to user page table
    #   - sscratch holds TRAPFRAME VA (set by user_trap_ret)
    #   - all GP registers hold user values
    csrrw a0, sscratch, a0
    sd    x1,   40(a0)     # ra
    sd    x2,   48(a0)     # sp
    sd    x3,   56(a0)     # gp
    sd    x4,   64(a0)     # tp
    sd    x5,   72(a0)     # t0
    sd    x6,   80(a0)     # t1
    sd    x7,   88(a0)     # t2
    sd    x8,   96(a0)     # s0/fp
    sd    x9,  104(a0)     # s1
    # (save x10/a0 later)
    sd    x11, 120(a0)     # a1
    sd    x12, 128(a0)     # a2
    sd    x13, 136(a0)     # a3
    sd    x14, 144(a0)     # a4
    sd    x15, 152(a0)     # a5
    sd    x16, 160(a0)     # a6
    sd    x17, 168(a0)     # a7
    sd    x18, 176(a0)     # s2
    sd    x19, 184(a0)     # s3
    sd    x20, 192(a0)     # s4
    sd    x21, 200(a0)     # s5
    sd    x22, 208(a0)     # s6
    sd    x23, 216(a0)     # s7
    sd    x24, 224(a0)     # s8
    sd    x25, 232(a0)     # s9
    sd    x26, 240(a0)     # s10
    sd    x27, 248(a0)     # s11
    sd    x28, 256(a0)     # t3
    sd    x29, 264(a0)     # t4
    sd    x30, 272(a0)     # t5
    sd    x31, 280(a0)     # t6

    csrr  t0, sscratch
    sd    t0, 112(a0)

    ld    t0, 0(a0)        # kernel_satp
    ld    sp, 8(a0)        # kernel_sp
    ld    t1, 16(a0)       # user_trap
    ld    tp, 24(a0)       # hartid

    csrw  satp, t0;
    sfence.vma zero, zero

    jr    t1

.align 4
user_ret:
    # Arguments (passed by user_trap_ret via calling convention):
    #   a0 = TRAPFRAME VA (base pointer for register restore)
    #   a1 = user page table SATP value
    csrw  satp, a1
    sfence.vma zero, zero

    csrw  sscratch, a0
    ld    x1,   40(a0)     # ra
    ld    x2,   48(a0)     # sp
    ld    x3,   56(a0)     # gp
    ld    x4,   64(a0)     # tp
    ld    x5,   72(a0)     # t0
    ld    x6,   80(a0)     # t1
    ld    x7,   88(a0)     # t2
    ld    x8,   96(a0)     # s0/fp
    ld    x9,  104(a0)     # s1
    # (load x10/a0 later)
    ld    x11, 120(a0)     # a1
    ld    x12, 128(a0)     # a2
    ld    x13, 136(a0)     # a3
    ld    x14, 144(a0)     # a4
    ld    x15, 152(a0)     # a5
    ld    x16, 160(a0)     # a6
    ld    x17, 168(a0)     # a7
    ld    x18, 176(a0)     # s2
    ld    x19, 184(a0)     # s3
    ld    x20, 192(a0)     # s4
    ld    x21, 200(a0)     # s5
    ld    x22, 208(a0)     # s6
    ld    x23, 216(a0)     # s7
    ld    x24, 224(a0)     # s8
    ld    x25, 232(a0)     # s9
    ld    x26, 240(a0)     # s10
    ld    x27, 248(a0)     # s11
    ld    x28, 256(a0)     # t3
    ld    x29, 264(a0)     # t4
    ld    x30, 272(a0)     # t5
    ld    x31, 280(a0)     # t6

    sd    a0,  112(a0)

    sret
