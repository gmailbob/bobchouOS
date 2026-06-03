/*
 * errno.h — Error codes returned by system calls.
 *
 * Syscall handlers return negative error codes (e.g., -EFAULT).
 * Values match Linux for familiarity.
 *
 * See Lecture 6-2, Part 4.
 */

#ifndef ERRNO_H
#define ERRNO_H

// clang-format off
#define ENOSYS       38   /* syscall not implemented */
#define EFAULT       14   /* bad user address */
#define EBADF         9   /* bad file descriptor */
#define EINVAL       22   /* invalid argument */
#define ENOMEM       12   /* out of memory */
#define ENAMETOOLONG 36   /* filename too long */
#define ENOENT        2   /* no such file or directory */
// clang-format on

#endif /* ERRNO_H */
