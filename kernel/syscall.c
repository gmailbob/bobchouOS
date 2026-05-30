/*
 * syscall.c — System call dispatch and handlers.
 *
 * Dispatch table maps syscall number (a7) to handler function.
 * Each sys_* handler reads arguments from the trapframe and returns
 * an int64 (>= 0 success, < 0 negated error code).
 *
 * See Lecture 6-2, Parts 2, 3, 7.
 */

#include "proc.h"
#include "trapframe.h"
#include "syscall_num.h"
#include "errno.h"
#include "drivers/uart.h"

/* --- Syscall handlers --- */

/*
 * sys_write — write bytes from a user buffer to a file descriptor.
 *
 * Args (from trapframe):
 *   a0 = fd (only fd=1/stdout supported for now)
 *   a1 = user buffer address
 *   a2 = length
 *
 * Returns bytes written, or negative error code.
 */
static int64
sys_write(void) {
    /* TODO: read fd, uaddr, len from trapframe */
    /* TODO: validate fd (only 1 is valid) */
    /* TODO: validate len (reject negative or > 1024) */
    /* TODO: loop: copyin chunks into kbuf, uart_putc each byte */
    /* TODO: return bytes written */
    return -ENOSYS;
}

/*
 * sys_exit — terminate the current process.
 *
 * Args (from trapframe):
 *   a0 = exit status
 *
 * Never returns.
 */
static int64
sys_exit(void) {
    /* TODO: read status from trapframe */
    /* TODO: call proc_exit(status) */
    return 0; /* unreachable */
}

/* --- Dispatch table --- */

static int64 (*syscalls[])(void) = {
    [0]         = 0,
    [SYS_write] = sys_write,
    [SYS_exit]  = sys_exit,
};

/*
 * syscall — dispatch a system call.
 *
 * Reads the syscall number from a7, bounds-checks, and calls the handler.
 * Returns the handler's result (or -ENOSYS for invalid numbers).
 * The caller writes the return value to trapframe->a0.
 */
int64
syscall(void) {
    struct proc *p = this_proc();
    int num = (int)p->trapframe->a7;

    if (num > 0 && num < NSYSCALL && syscalls[num])
        return syscalls[num]();
    return -ENOSYS;
}
