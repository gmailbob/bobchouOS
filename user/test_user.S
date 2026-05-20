# user/test_user.S — Minimal test user program for Round 6-1.
#
# Executes in U-mode. Issues ecall to trap into kernel.
# Kernel prints the arguments and kills the process.
#
# See Lecture 6-1, Part 8.

.section .text
.globl _start

_start:
    li  a7, 0       # syscall number (placeholder — no real dispatch yet)
    li  a0, 42      # argument (kernel will print this)
    ecall           # trap into kernel

    # Should never reach here — kernel kills the process after ecall
spin:
    j   spin
