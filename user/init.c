/*
 * init.c — First user process (PID 1).
 *
 * Structure mirrors a real init:
 *   1. Spawn child programs (hello for now, shell in Phase 8)
 *   2. Reap orphaned zombies forever
 *
 * See Lecture 6-3, Part 9.
 */

#include "user.h"

int
main(void) {
    /* Spawn hello as a one-time test */
    int pid = fork();
    if (pid == 0) {
        char *argv[] = {"hello", 0};
        exec("hello", argv);
        exit(1);
    }

    /* Reap children forever (init must never exit) */
    for (;;) {
        int status;
        wait(&status);
    }
}
