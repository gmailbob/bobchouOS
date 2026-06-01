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
    struct proc *p = this_proc();
    int fd = (int)p->trapframe->a0;
    uint64 uaddr = p->trapframe->a1;
    uint64 len = p->trapframe->a2;

    if (fd != 1)
        return -EBADF;
    if (len > 1024)
        return -EINVAL;

    char kbuf[128];
    uint64 written = 0;
    while (written < len) {
        uint64 n = len - written;
        if (n > sizeof(kbuf))
            n = sizeof(kbuf);
        if (copyin(p->pagetable, kbuf, uaddr + written, n) < 0)
            return -EFAULT;
        for (uint64 i = 0; i < n; i++)
            uart_putc(kbuf[i]);
        written += n;
    }
    return (int64)written;
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
    proc_exit((int)this_proc()->trapframe->a0);
    return 0; /* unreachable */
}

/* --- Dispatch table --- */
// clang-format off
static int64 (*syscalls[])(void) = {
    [0]         = 0,
    [SYS_write] = sys_write,
    [SYS_exit]  = sys_exit,
};
// clang-format on

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
