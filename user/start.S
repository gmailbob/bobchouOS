/*
 * start.S — C runtime entry point for user programs.
 *
 * Calls main(argc, argv) and then exits with main's return value.
 * argc is in a0, argv is in a1 (set up by exec).
 *
 * See Lecture 6-3, Part 9.
 */

#include "syscall_num.h"

.section .text
.globl _start
_start:
    call main
    /* main returned — exit with its return value (still in a0) */
    li a7, SYS_exit
    ecall
    /* unreachable */
