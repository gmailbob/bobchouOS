/*
 * syscall_num.h — System call numbers (user/kernel ABI contract).
 *
 * Shared between kernel and user programs. Numbers are sequential
 * starting at 1; Linux RISC-V equivalents noted in comments.
 *
 * See Lecture 6-2, Part 2 and Lecture 6-3, Part 7.
 */

#ifndef SYSCALL_NUM_H
#define SYSCALL_NUM_H

// clang-format off
#define SYS_write   1    /* Linux RISC-V: 64 */
#define SYS_exit    2    /* Linux RISC-V: 93 */
#define SYS_fork    3    /* Linux RISC-V: 220 (clone) */
#define SYS_exec    4    /* Linux RISC-V: 221 (execve) */
#define SYS_wait    5    /* Linux RISC-V: 260 (wait4) */
#define SYS_getpid  6    /* Linux RISC-V: 172 */
#define SYS_kill    7    /* Linux RISC-V: 129 */
#define SYS_sleep   8    /* Linux RISC-V: 101 (nanosleep) */

#define NSYSCALL    9    /* one past the last valid syscall number */
// clang-format on

#endif /* SYSCALL_NUM_H */
