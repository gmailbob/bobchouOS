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
    # TODO: read mhartid into a register
    # TODO: branch to 'park' if not zero

    # ---- Step 2: Zero BSS ----
    # Load _bss_start and _bss_end from linker script symbols.
    # Loop: store 8 bytes of zeros at a time until start >= end.
    #
    # TODO: load _bss_start address into a register
    # TODO: load _bss_end address into a register
bss_clear:
    # TODO: if start >= end, jump to bss_done
    # TODO: store zero (8 bytes) at the current address
    # TODO: advance pointer by 8
    # TODO: jump back to bss_clear
bss_done:

    # ---- Step 3: Set up stack ----
    # Load the stack_top symbol (defined in linker.ld) into sp.
    #
    # TODO: load stack_top into sp

    # ---- Step 4: Jump to C ----
    # Call kmain(). If it returns, fall through to spin.
    #
    # TODO: call kmain

spin:
    wfi
    j     spin

park:
    wfi
    j     park
