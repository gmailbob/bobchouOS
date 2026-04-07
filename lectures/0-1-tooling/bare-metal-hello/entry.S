# entry.S — Bare-metal entry point for RISC-V
#
# This is the first code the CPU executes after QEMU loads our binary.
# QEMU's "virt" machine jumps to address 0x80000000, which is where the
# linker script places this code.
#
# What we need to do before calling C:
#   1. Set up the stack pointer (sp). C functions use the stack for local
#      variables, function arguments, and return addresses. Without a valid
#      sp, any C function call will corrupt random memory.
#   2. Jump to main().
#
# That's it. No interrupts, no virtual memory, no multi-core setup — just
# enough to get into C.

.section .text.entry      # Place this in a special section so the linker
                          # script can put it first (at 0x80000000).
.global _start

_start:
    # Load the address of the top of our stack into sp.
    # The stack grows downward on RISC-V, so we point sp to the *top*
    # of the stack region (defined in the linker script).
    #
    # How does this file know about "stack_top"? It doesn't — yet.
    # The assembler generates a placeholder here ("I need the address of
    # stack_top"), and the linker fills it in later. The linker script
    # defines stack_top = . (the current address after the 4KB stack
    # region), and the linker matches that definition to this reference.
    # Same mechanism as calling a function defined in another .c file.
    #
    # Why assembly? Every compiled C function uses sp implicitly — the
    # compiler generates prologue/epilogue code (addi sp, sp, -N / sd ra,
    # ...) that assumes sp already points to valid memory. Without a
    # valid sp, the first C function call writes to a random address.
    # C can't set up its own stack (chicken-and-egg), so assembly
    # breaks the cycle.
    la sp, stack_top

    # Jump to our C main function.
    # We use 'call' (which is a pseudo-instruction for auipc+jalr) rather
    # than 'j' (plain jump) because 'call' saves the return address in ra.
    # Not that main() should ever return — but it's good practice.
    call main

    # If main() ever returns, spin forever. A bare-metal program has nowhere
    # to "return to", so we just halt in an infinite loop.
spin:
    wfi                   # Wait For Interrupt — puts the CPU in low-power
                          # state instead of busy-spinning.
    j spin
