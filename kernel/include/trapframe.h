/*
 * trapframe.h — Per-process trapframe structure.
 *
 * The trapframe is a page of memory that stores:
 * 1. Kernel bootstrap values (written by user_trap_ret before entering user mode)
 * 2. User register state (saved by user_vec on trap, restored by user_ret)
 *
 * Field offsets must match the sd/ld offsets in trampoline.S.
 * See Lecture 6-1, Part 3.
 */

#ifndef TRAPFRAME_H
#define TRAPFRAME_H

#include "types.h"

// clang-format off
struct trapframe {
    /* Kernel bootstrap data (read by user_vec on trap entry) */
    uint64 kernel_satp;     /* offset 0:  kernel page table SATP value */
    uint64 kernel_sp;       /* offset 8:  this process's kernel stack top */
    uint64 user_trap;       /* offset 16: address of user_trap() */
    uint64 hartid;          /* offset 24: hart ID (loaded into tp) */

    /* Saved user PC (written by user_trap, read by user_trap_ret) */
    uint64 epc;             /* offset 32: saved sepc */

    /* User general-purpose registers x1–x31, in register-number order.
     * ABI names used for C readability; assembly uses x1–x31 with comments. */
    uint64 ra;              /* x1,  offset 40 */
    uint64 sp;              /* x2,  offset 48 */
    uint64 gp;              /* x3,  offset 56 */
    uint64 tp;              /* x4,  offset 64 */
    uint64 t0;              /* x5,  offset 72 */
    uint64 t1;              /* x6,  offset 80 */
    uint64 t2;              /* x7,  offset 88 */
    uint64 s0;              /* x8,  offset 96 */
    uint64 s1;              /* x9,  offset 104 */
    uint64 a0;              /* x10, offset 112 */
    uint64 a1;              /* x11, offset 120 */
    uint64 a2;              /* x12, offset 128 */
    uint64 a3;              /* x13, offset 136 */
    uint64 a4;              /* x14, offset 144 */
    uint64 a5;              /* x15, offset 152 */
    uint64 a6;              /* x16, offset 160 */
    uint64 a7;              /* x17, offset 168 */
    uint64 s2;              /* x18, offset 176 */
    uint64 s3;              /* x19, offset 184 */
    uint64 s4;              /* x20, offset 192 */
    uint64 s5;              /* x21, offset 200 */
    uint64 s6;              /* x22, offset 208 */
    uint64 s7;              /* x23, offset 216 */
    uint64 s8;              /* x24, offset 224 */
    uint64 s9;              /* x25, offset 232 */
    uint64 s10;             /* x26, offset 240 */
    uint64 s11;             /* x27, offset 248 */
    uint64 t3;              /* x28, offset 256 */
    uint64 t4;              /* x29, offset 264 */
    uint64 t5;              /* x30, offset 272 */
    uint64 t6;              /* x31, offset 280 */
};
// clang-format on

#endif /* TRAPFRAME_H */
