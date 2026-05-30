/*
 * syscall_num.h — System call numbers (user/kernel ABI contract).
 *
 * Shared between kernel and user programs. Numbers are sequential
 * starting at 1; Linux RISC-V equivalents noted in comments.
 *
 * See Lecture 6-2, Part 2.
 */

#ifndef SYSCALL_NUM_H
#define SYSCALL_NUM_H

#define SYS_write   1    /* Linux RISC-V: 64 */
#define SYS_exit    2    /* Linux RISC-V: 93 */

#define NSYSCALL    3    /* one past the last valid syscall number */

#endif /* SYSCALL_NUM_H */
