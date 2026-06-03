/*
 * user.h — User-space syscall prototypes.
 *
 * Thin C declarations for the assembly stubs in usys.S.
 */

#ifndef USER_H
#define USER_H

/* Syscall wrappers (implemented in usys.S) */
int write(int fd, const void *buf, int len);
void exit(int status) __attribute__((noreturn));
int fork(void);
int exec(const char *path, char **argv);
int wait(int *status);
int getpid(void);
int kill(int pid);
int sleep(int ticks);

#endif /* USER_H */
