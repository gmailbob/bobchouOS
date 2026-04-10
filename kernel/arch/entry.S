# entry.S — Kernel entry point for bobchouOS
#
# This is the very first code that executes when QEMU jumps to 0x80000000.
# It runs in M-mode. Its job is to set up just enough for C code to run,
# then hand off to kmain().
#
# Four things to do:
#   1. Park non-boot harts (only hart 0 should continue)
#   2. Zero the BSS section (so uninitialized globals start at zero)
#   3. Set up the stack pointer
#   4. Jump to kmain()
#
# Refer to Lecture 1-1, Part 5 for a detailed walkthrough.

.section .text.entry
.global _start

_start:
    # ---- Step 1: Hart parking ----
    # Read this hart's ID from the mhartid CSR.
    # If it's not hart 0, jump to the 'park' label.
    #
    csrr t0, mhartid
    bnez t0, park

    # ---- Step 2: Zero BSS ----
    # Load _bss_start and _bss_end from linker script symbols.
    # Loop: store 8 bytes of zeros at a time until start >= end.
    #
    la t0, _bss_start
    la t1, _bss_end
bss_clear:
    bge t0, t1, bss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j bss_clear
bss_done:

    # ---- Step 3: Set up stack ----
    # Load the stack_top symbol (defined in linker.ld) into sp.
    #
    la sp, stack_top

    # ---- Step 4: Jump to C ----
    # Call kmain(). If it returns, fall through to spin.
    #
    call kmain

spin:
    wfi
    j     spin

park:
    wfi
    j     park
