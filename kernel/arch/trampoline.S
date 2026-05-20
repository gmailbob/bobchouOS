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
.globl trampoline
.globl user_vec
.globl user_ret
.align 4

trampoline:
user_vec:
    # TODO: Trap from user space.
    #
    # State on entry:
    #   - CPU is in S-mode (hardware switched on trap)
    #   - satp still points to user page table
    #   - sscratch holds TRAPFRAME VA (set by user_trap_ret)
    #   - all GP registers hold user values
    #
    # Steps:
    # 1. csrrw a0, sscratch, a0 — swap a0 with trapframe pointer
    # 2. Save all 31 user GP registers into trapframe (a0 as base)
    #    - Save x1-x9, x11-x31 directly
    #    - Save original a0 (now in sscratch) last: csrr t0, sscratch; sd t0, 112(a0)
    # 3. Load kernel bootstrap values from trapframe:
    #    - sp  ← offset 8  (kernel_sp)
    #    - tp  ← offset 24 (hartid)
    #    - t1  ← offset 16 (user_trap function address)
    #    - t0  ← offset 0  (kernel_satp)
    # 4. Switch page table: csrw satp, t0; sfence.vma zero, zero
    # 5. Jump to C handler: jr t1

    j user_vec      # placeholder — replace with implementation

.align 4
user_ret:
    # TODO: Return to user mode.
    #
    # Arguments (passed by user_trap_ret via calling convention):
    #   a0 = TRAPFRAME VA (base pointer for register restore)
    #   a1 = user page table SATP value
    #
    # Steps:
    # 1. Switch to user page table: csrw satp, a1; sfence.vma zero, zero
    # 2. Store trapframe VA in sscratch for next trap: csrw sscratch, a0
    # 3. Restore all 31 user GP registers from trapframe (a0 as base)
    #    - Restore x1-x9, x11-x31
    #    - Restore a0 (x10) last
    # 4. sret — return to user mode (PC = sepc, mode = U)

    j user_ret      # placeholder — replace with implementation
