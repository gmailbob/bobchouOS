/*
 * hello.c — Test program that exercises argv, write, and sleep.
 *
 * Prints "hello, <name>!" using argv[1] (or "world" if no arg).
 * Then sleeps 1 second and exits — proving the full syscall pipeline.
 *
 * See Lecture 6-3, Part 9.
 */

#include "user.h"

int
main(int argc, char *argv[]) {
    char *name = (argc > 1) ? argv[1] : "world";

    write(1, "hello, ", 7);
    /* write the name (compute length manually — no strlen in user libc yet) */
    int len = 0;
    while (name[len])
        len++;
    write(1, name, len);
    write(1, "!\n", 2);

    sleep(1000); /* 1 second */
    write(1, "exiting after 1s sleep\n", 23);
    exit(0);
}
