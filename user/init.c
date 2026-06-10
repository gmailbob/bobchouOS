/*
 * init.c — First user process (PID 1).
 *
 * Spawns utest, then reaps children forever.
 *
 * See Lecture 6-3/6-4.
 */

#include "user.h"

int
main(void) {
    int pid = fork();
    if (pid == 0) {
        char *argv[] = {"utest", 0};
        exec("utest", argv);
        exit(1);
    }

    /* Reap children forever (init must never exit) */
    for (;;) {
        int status;
        wait(&status);
    }
}
