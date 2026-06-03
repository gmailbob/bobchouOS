/*
 * hello.c — Simple test program that prints, sleeps, and exits.
 *
 * See Lecture 6-3, Part 9.
 */

#include "user.h"

int
main(void) {
    write(1, "hello world\n", 12);
    sleep(10); /* ~1 second (10 timer ticks at 100ms interval) */
    write(1, "exiting after sleep 10\n", 23);
    exit(0);
}
