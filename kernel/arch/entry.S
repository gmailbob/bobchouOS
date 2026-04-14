# entry.S — M-mode boot and timer code for bobchouOS
#
# This is the only file that runs in M-mode. All other kernel code
# runs in S-mode after mret. Any future M-mode code goes here.
#
# Boot sequence: set up hardware, configure timer, mret to kmain().
# Timer handler (timer_vec): schedules next tick, raises SSIP.
#
# M-mode CSR usage:
#   mtvec     — set to timer_vec (our only M-mode handler)
#   mscratch  — points to timer_scratch (register save area for timer_vec)
#   mie       — only MTIE enabled; timer is the sole M-mode interrupt
#   mepc      — hardware-managed (written on trap entry, read by mret)
#   mcause    — never read; MTIE is the only enabled source, so
#               timer_vec skips dispatch and handles the tick directly
#   mtval     — never read; no need for trap context in the timer handler
#
# Refer to Lectures 1-1, 2-1, and 2-3.

#include "riscv.h"

.section .text.entry
.global _start

_start:
    # ---- Step 1: Hart parking ----
    # Read this hart's ID from the mhartid CSR.
    # If it's not hart 0, jump to the 'park' label.
    csrr  t0, mhartid
    bnez  t0, park

    # ---- Step 2: Zero BSS ----
    # Load _bss_start and _bss_end from linker script symbols.
    # Loop: store 8 bytes of zeros at a time until start >= end.
    la    t0, _bss_start
    la    t1, _bss_end
bss_clear:
    bgeu  t0, t1, bss_done
    sd    zero, 0(t0)
    addi  t0, t0, 8
    j     bss_clear
bss_done:

    # ---- Step 3: Set up stack ----
    # Load the stack_top symbol (defined in linker.ld) into sp.
    la    sp, stack_top

    # ---- Step 4: Set mstatus.MPP to Supervisor ----
    # Read mstatus, clear the MPP field (bits 12:11), set it to 01
    # (Supervisor), and write it back. This tells mret which mode
    # to switch to.
    csrr  t0, mstatus
    li    t1, ~MSTATUS_MPP_MASK
    and   t0, t0, t1
    li    t1, MSTATUS_MPP_S
    or    t0, t0, t1
    csrw  mstatus, t0

    # ---- Step 5: Set mepc to kmain ----
    # Load the address of kmain into mepc. After mret, the CPU will
    # jump here.
    la    t0, kmain
    csrw  mepc, t0

    # ---- Step 6: Disable paging ----
    # Write 0 to satp so there is no address translation.
    csrw  satp, zero

    # ---- Step 7: Delegate traps to S-mode ----
    # Write 0xffff to both medeleg and mideleg so that S-mode handles
    # its own exceptions and interrupts. Bits that can't be delegated
    # are hardwired to 0 by the hardware.
    li    t0, 0xffff
    csrw  medeleg, t0
    csrw  mideleg, t0

    # ---- Step 8: Configure PMP ----
    # Grant S-mode access to the entire physical address space.
    li    t0, PMP_NAPOT_ALL
    csrw  pmpaddr0, t0
    li    t0, PMPCFG_TOR_RWX
    csrw  pmpcfg0, t0

    # ---- Step 9: Set up timer ----
    # Program the CLINT so periodic timer interrupts reach S-mode
    # via the SSIP forwarding trick (see Lecture 2-3).

    # Fill the scratch area for timer_vec:
    #   scratch[0..2] = register save slots (used at runtime)
    #   scratch[3]    = &mtimecmp[0]
    #   scratch[4]    = timer interval
    la    t0, timer_scratch
    li    t1, CLINT_MTIMECMP(0)
    sd    t1, 24(t0)
    li    t2, TIMER_INTERVAL
    sd    t2, 32(t0)

    # Point mscratch at the scratch area.
    csrw  mscratch, t0

    # Program the first deadline: mtimecmp = mtime + interval.
    li    t0, CLINT_MTIME
    ld    t0, 0(t0)            # t0 = current mtime
    add   t0, t0, t2           # t0 = mtime + interval
    sd    t0, 0(t1)            # mtimecmp[0] = first deadline

    # Set mtvec to timer_vec (direct mode — .align 4 gives MODE=00).
    la    t0, timer_vec
    csrw  mtvec, t0

    # Enable machine timer interrupt only (see header for rationale).
    li    t0, MIE_MTIE
    csrs  mie, t0

    # ---- Step 10: Enter S-mode ----
    # Execute mret. The CPU reads mepc (→ kmain) and mstatus.MPP
    # (→ Supervisor) and switches mode + jumps in one instruction.
    mret

park:
    wfi
    j     park

# ---------- M-mode timer interrupt handler ----------
#
# timer_vec — M-mode handler for machine timer interrupts.
# Schedules the next tick (mtimecmp += interval) and raises SSIP
# to forward the event to S-mode. Uses mscratch to save/restore
# three registers without touching the stack.
# Refer to Lecture 2-3, Part 4.

.align 4
.globl timer_vec

timer_vec:
    csrrw a0, mscratch, a0     # a0 = scratch area, mscratch = old a0
    sd    a1, 0(a0)            # save a1
    sd    a2, 8(a0)            # save a2
    sd    a3, 16(a0)           # save a3

    # Schedule next tick: mtimecmp += interval
    ld    a1, 24(a0)           # a1 = &mtimecmp[0]
    ld    a2, 32(a0)           # a2 = interval
    ld    a3, 0(a1)            # a3 = current mtimecmp
    add   a3, a3, a2           # a3 = next deadline
    sd    a3, 0(a1)            # write new mtimecmp

    # Raise supervisor software interrupt to notify S-mode.
    li    a1, SIP_SSIP
    csrs  sip, a1

    ld    a1, 0(a0)            # restore a1
    ld    a2, 8(a0)            # restore a2
    ld    a3, 16(a0)           # restore a3
    csrrw a0, mscratch, a0     # restore a0
    mret

# ---------- Timer scratch area (BSS) ----------

.section .bss
.align 4
.globl timer_scratch

timer_scratch:
    .zero 40          # 5 x 8 bytes (see Lecture 2-3, Part 4)
