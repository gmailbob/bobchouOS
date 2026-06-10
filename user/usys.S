/*
 * usys.S — User-space system call stubs.
 *
 * Each stub loads the syscall number into a7 and traps into the kernel
 * via ecall. Arguments are already in a0-a5 per calling convention.
 *
 * See Lecture 6-3, Part 9.
 */

#include "syscall_num.h"

.globl write
write:
    li a7, SYS_write
    ecall
    ret

.globl exit
exit:
    li a7, SYS_exit
    ecall
    /* unreachable */

.globl fork
fork:
    li a7, SYS_fork
    ecall
    ret

.globl exec
exec:
    li a7, SYS_exec
    ecall
    ret

.globl wait
wait:
    li a7, SYS_wait
    ecall
    ret

.globl getpid
getpid:
    li a7, SYS_getpid
    ecall
    ret

.globl kill
kill:
    li a7, SYS_kill
    ecall
    ret

.globl sleep
sleep:
    li a7, SYS_sleep
    ecall
    ret

.globl sbrk
sbrk:
    li a7, SYS_sbrk
    ecall
    ret
