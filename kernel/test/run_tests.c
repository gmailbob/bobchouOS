/*
 * run_tests.c -- Test runner for bobchouOS.
 *
 * Calls each test suite and prints a summary.
 * Add new test_*() calls here as suites are created.
 */

#include "test/test.h"

int test_pass = 0;
int test_fail = 0;

/* Test suite declarations — each lives in its own file. */
void test_kprintf(void);
void test_string(void);
void test_trap(void);

void
run_tests(void) {
    kprintf("\n=== bobchouOS test suite ===\n\n");

    test_kprintf();
    test_string();
    test_trap();

    kprintf("\n=== results: %d passed, %d failed ===\n\n", test_pass, test_fail);
}
