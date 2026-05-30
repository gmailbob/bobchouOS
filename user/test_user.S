# user/test_user.S — Hello-world user program for Round 6-2.
#
# Prints "hello world\n" via write() syscall, then exits cleanly.
#
# See Lecture 6-2, Part 9.

#include "syscall_num.h"

.section .text
.globl _start

_start:
    # write(1, msg, 12)
    li  a7, SYS_write
    li  a0, 1           # fd = stdout
    la  a1, msg         # buffer address (user VA)
    li  a2, 12          # length
    ecall

    # exit(0)
    li  a7, SYS_exit
    li  a0, 0           # status = 0
    ecall

    # unreachable
spin:
    j   spin

.section .rodata
msg:
    .ascii "hello world\n"
