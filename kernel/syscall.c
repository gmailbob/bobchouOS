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
#include "drivers/uart.h"
#include "errno.h"
#include "string.h"
#include "syscall_num.h"
#include "trapframe.h"

/* Upper bound on sleep() milliseconds — caps wake_time arithmetic so
 * read_time() + ms*MTIME_FREQ/1000 cannot overflow uint64. One hour is
 * far beyond anything our programs need. */
#define SLEEP_MS_MAX (60ULL * 60ULL * 1000ULL)

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

/*
 * sys_fork — create a child process (copy of the caller).
 *
 * Returns child PID to parent, 0 to child.
 */
static int64
sys_fork(void) {
    return proc_fork();
}

/*
 * sys_exec — replace address space with a new program.
 *
 * Args (from trapframe):
 *   a0 = user pointer to path string
 *   a1 = user pointer to argv array (null-terminated)
 *
 * Returns argc on success (never returns to old code),
 * -1 on failure (old process continues).
 */
static int64
sys_exec(void) {
    struct proc *p = this_proc();
    uint64 upath = p->trapframe->a0;
    uint64 uargv = p->trapframe->a1;

    char path[64];
    if (copyinstr(p->pagetable, path, upath, sizeof(path)) < 0)
        return -EFAULT;

    char kargv_buf[512];
    int off = 0;
    char *argv[16];
    int argc = 0;
    for (; argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1; argc++) {
        uint64 arg_va;
        if (copyin(p->pagetable, &arg_va, uargv + argc * 8, 8) < 0)
            return -EFAULT;
        if (arg_va == 0)
            break;
        argv[argc] = kargv_buf + off;
        if (copyinstr(p->pagetable, argv[argc], arg_va, sizeof(kargv_buf) - off) < 0)
            return -EFAULT;
        off += strlen(argv[argc]) + 1;
    }
    argv[argc] = 0;

    return proc_exec(path, argv);
}

/*
 * sys_wait — wait for a child to exit and reap it.
 *
 * Args (from trapframe):
 *   a0 = user pointer to int (for exit status), or 0 to ignore
 *
 * Returns child PID, or -1 if no children.
 */
static int64
sys_wait(void) {
    int kstatus;
    int pid = proc_wait(&kstatus);
    struct proc *p = this_proc();
    if (pid > 0 && p->trapframe->a0)
        if (copyout(p->pagetable, p->trapframe->a0, &kstatus, sizeof(int)) < 0)
            return -EFAULT;
    return pid;
}

/*
 * sys_getpid — return the current process's PID.
 */
static int64
sys_getpid(void) {
    return this_proc()->pid;
}

/*
 * sys_kill — send a kill signal to a process.
 *
 * Args (from trapframe):
 *   a0 = target PID
 *
 * Returns 0 on success, -1 if PID not found.
 */
static int64
sys_kill(void) {
    int pid = (int)this_proc()->trapframe->a0;
    return proc_kill(pid);
}

/*
 * sys_sleep — suspend the calling process for a duration in milliseconds.
 *
 * Args (from trapframe):
 *   a0 = milliseconds to sleep
 *
 * Returns 0 on success.
 *
 * Design: sorted tsleep_list + precise hardware arming. See Lecture 6-3, Part 7.
 * Lock ordering: tsleep_lock → p->lock (same as wake_expired_sleepers, proc_kill).
 */
static int64
sys_sleep(void) {
    struct proc *p = this_proc();
    uint64 ms = p->trapframe->a0;

    /* Clamp ms so read_time() + ms*ticks_per_ms can't overflow uint64.
     * A hostile/huge ms would otherwise wrap wake_time to a tiny value and
     * wake the proc almost immediately. MS_MAX ticks fits comfortably. */
    uint64 ticks = MS_TO_MTIME(ms < SLEEP_MS_MAX ? ms : SLEEP_MS_MAX);
    p->wake_time = read_time() + ticks;

    /* Insert into tsleep_list sorted by wake_time (earliest first).
     * Lock ordering: tsleep_lock → p->lock. */
    unsigned long irq;
    spin_lock_irqsave(&tsleep_lock, &irq);

    struct list_head *insert_point = &tsleep_list;
    struct proc *pos;
    list_for_each_entry(pos, &tsleep_list, tsleep_link) {
        if (pos->wake_time > p->wake_time) {
            insert_point = &pos->tsleep_link;
            break;
        }
    }
    list_add_tail(&p->tsleep_link, insert_point);

    /* Golden rule: only p->lock crosses the sched boundary. */
    spin_lock(&p->lock);
    p->state = PROC_SLEEPING;
    spin_unlock(&tsleep_lock); /* plain: keep irqs off for sched */

    sched(); /* outbound: p->lock crosses to scheduler */

    /* Inbound: scheduler re-acquired p->lock before dispatching us.
     * Release it and restore interrupt state. */
    p->wake_time = 0;
    spin_unlock_irqrestore(&p->lock, irq);
    return 0;
}

/*
 * sys_sbrk — grow or shrink the process heap.
 *
 * Args (from trapframe):
 *   a0 = n (signed: positive = grow, negative = shrink, 0 = query)
 *
 * Returns old break address on success, -1 on failure.
 */
static int64
sys_sbrk(void) {
    return proc_sbrk((int64)this_proc()->trapframe->a0);
}

/* --- Dispatch table --- */
// clang-format off
static int64 (*syscalls[])(void) = {
    [0]          = 0,
    [SYS_write]  = sys_write,
    [SYS_exit]   = sys_exit,
    [SYS_fork]   = sys_fork,
    [SYS_exec]   = sys_exec,
    [SYS_wait]   = sys_wait,
    [SYS_getpid] = sys_getpid,
    [SYS_kill]   = sys_kill,
    [SYS_sleep]  = sys_sleep,
    [SYS_sbrk]   = sys_sbrk,
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
