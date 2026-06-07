/*
 * hello.c — Simple test program that prints, sleeps, and exits.
 *
 * See Lecture 6-3, Part 9.
 */

#include "user.h"

int
main(void) {
    write(1, "hello world\n", 12);
    sleep(1000); /* 1 second */
    write(1, "exiting after 1s sleep\n", 23);
    exit(0);
}
