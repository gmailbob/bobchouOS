# entry.S — M-mode boot and timer code for bobchouOS
#
# This is the only file that runs in M-mode. All other kernel code
# runs in S-mode after mret. Any future M-mode code goes here.
#
# It contains:
#
# Boot sequence (runs once at startup):
#   1. Park non-boot harts (only hart 0 should continue)
#   2. Zero the BSS section (so uninitialized globals start at zero)
#   3. Set up the stack pointer
#   4. Configure M-mode CSRs for the M→S switch
#   5. Delegate traps to S-mode
#   6. Set up PMP so S-mode can access all memory
#   7. Set up timer (CLINT, scratch area, mtvec, mie)
#   8. Execute mret to enter S-mode at kmain()
#
# Timer handler (runs on each timer interrupt):
#   timer_vec — M-mode handler that schedules the next tick and
#   raises SSIP to forward the interrupt to S-mode.
#
# Refer to Lecture 1-1 (Parts 1-3), Lecture 2-1 (Parts 3-7),
# and Lecture 2-3 (Parts 4-5).

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
    li    t1, ~(3 << 11)
    and   t0, t0, t1
    li    t1, (1 << 11)
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
    # Set pmpaddr0 to 0x3fffffffffffff (all 54 implemented bits = 1)
    # and pmpcfg0 to 0x0f (TOR mode, RWX). This grants S-mode access
    # to the entire physical address space.
    li    t0, 0x3fffffffffffff
    csrw  pmpaddr0, t0
    li    t0, 0x0f
    csrw  pmpcfg0, t0

    # ---- Step 9: Set up timer ----
    # Set up the CLINT timer so that periodic interrupts reach S-mode
    # via the SSIP forwarding trick.

    # 1. Fill the scratch area (timer_scratch) with pre-computed values:
    #    - scratch[3] = address of mtimecmp for hart 0
    #    - scratch[4] = timer interval
    #    (scratch[0..2] are save slots used by timer_vec at runtime)
    la    t0, timer_scratch
    li    t1, 0x2004000
    sd    t1, 24(t0)
    li    t2, 100000
    sd    t2, 32(t0)

    # 2. Point mscratch at the scratch area
    csrw  mscratch, t0

    # 3. Program the first timer deadline:
    #    Read mtime (MMIO), add the interval, write to mtimecmp (MMIO)
    li    t0, 0x200BFF8
    ld    t0, 0(t0)
    add   t0, t0, t2
    sd    t0, 0(t1)

    # 4. Set mtvec to timer_vec (direct mode, same .align trick as stvec)
    la    t0, timer_vec
    csrw  mtvec, t0

    # 5. Enable machine timer interrupt: set mie.MTIE (bit 7)
    li    t0, 1 << 7
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
# timer_vec — called when mtime >= mtimecmp (machine timer interrupt).
# This runs in M-mode. It must:
#   1. Swap a0 with mscratch to get the scratch area pointer
#   2. Save a1, a2, a3 to the scratch area
#   3. Schedule the next tick: mtimecmp = mtimecmp + interval
#      (load &mtimecmp and interval from the scratch area)
#   4. Raise SSIP (set sip bit 1) to notify S-mode
#   5. Restore a1, a2, a3 from the scratch area
#   6. Swap a0 back from mscratch
#   7. mret
#
# Refer to Lecture 2-3, Part 4.

.align 4
.globl timer_vec

timer_vec:
    csrrw a0, mscratch, a0
    sd    a1, 0(a0)
    sd    a2, 8(a0)
    sd    a3, 16(a0)
    ld    a1, 24(a0)
    ld    a2, 32(a0)
    ld    a3, 0(a1)

    add   a3, a3, a2
    sd    a3, 0(a1)

    li    a1, 1 << 1
    csrs  sip, a1

    ld    a1, 0(a0)
    ld    a2, 8(a0)
    ld    a3, 16(a0)
    csrrw a0, mscratch, a0
    mret

# ---------- Timer scratch area (BSS) ----------

.section .bss
.align 4
.globl timer_scratch

timer_scratch:
    .zero 40          # 5 x 8 bytes (see Lecture 2-3, Part 4)
