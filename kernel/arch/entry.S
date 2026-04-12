# entry.S — Kernel entry point for bobchouOS
#
# This is the very first code that executes when QEMU jumps to 0x80000000.
# It runs in M-mode. Its job is to set up just enough for C code to run,
# then switch to S-mode and hand off to kmain().
#
# Seven things to do:
#   1. Park non-boot harts (only hart 0 should continue)
#   2. Zero the BSS section (so uninitialized globals start at zero)
#   3. Set up the stack pointer
#   4. Configure M-mode CSRs for the M→S switch
#   5. Delegate traps to S-mode
#   6. Set up PMP so S-mode can access all memory
#   7. Execute mret to enter S-mode at kmain()
#
# Refer to Lecture 1-1 (Parts 1-3) and Lecture 2-1 (Parts 3-7).

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

    # ---- Step 9: Enter S-mode ----
    # Execute mret. The CPU reads mepc (→ kmain) and mstatus.MPP
    # (→ Supervisor) and switches mode + jumps in one instruction.
    mret

spin:
    wfi
    j     spin

park:
    wfi
    j     park
