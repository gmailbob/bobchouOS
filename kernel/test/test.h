/*
 * test.h -- Lightweight kernel test framework for bobchouOS.
 *
 * Usage:
 *   void test_foo(void) {
 *       TEST_ASSERT(1 + 1 == 2, "basic addition");
 *       TEST_ASSERT(memset(buf, 0, 8) == buf, "memset returns dst");
 *   }
 *
 * Build with: make test (adds -DRUN_TESTS)
 * Normal builds (make / make run) exclude all test code.
 */

#ifndef TEST_H
#define TEST_H

#include "kprintf.h"

/* Counters — defined in run_tests.c */
extern int test_pass;
extern int test_fail;

/*
 * Assert a condition. On failure, print file:line and the message.
 * Tests continue after failure (no abort) so you see all results.
 */
#define TEST_ASSERT(cond, msg)                                                                     \
    do {                                                                                           \
        if (cond) {                                                                                \
            test_pass++;                                                                           \
        } else {                                                                                   \
            kprintf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                              \
            test_fail++;                                                                           \
        }                                                                                          \
    } while (0)

/* Run all registered test suites. Called from kmain when RUN_TESTS is defined. */
void run_tests(void);

#endif /* TEST_H */
