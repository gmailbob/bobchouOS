/*
 * run_integration_tests.c — Scheduler-based integration test runner.
 *
 * Runs as a kernel process so tests can sleep, wake, fork, exit, and
 * exercise anything that needs a real scheduling context.
 */

#include "test/test.h"
#include "proc.h"
#include "sbi.h"

void test_proc(void);
void test_wait_queue(void);
void test_virtio_blk(void);
void test_spinlock_contention(void);
void test_sleeplock(void);
void test_bio(void);

void
run_integration_tests(void) {
    kthread_start();

    /* test_pass/test_fail are shared globals already carrying the unit-test
     * tallies (unit tests ran first, at boot). Snapshot them here so we can
     * report integration-only counts as the delta, then a grand total. */
    int unit_pass = test_pass;
    int unit_fail = test_fail;

    kprintf("\n=== integration tests ===\n\n");

    test_proc();
    test_wait_queue();
    test_spinlock_contention();
    test_virtio_blk();
    test_sleeplock();
    test_bio();

    kprintf("\n=== integration: %d passed, %d failed ===\n", test_pass - unit_pass,
            test_fail - unit_fail);
    kprintf("=== total: %d passed, %d failed ===\n", test_pass, test_fail);

    /* The helper threads these tests spawned are parented to init and never
     * reaped — they linger as zombies. Harmless here because we shut down
     * immediately; a long-running integration test would need real reaping. */
    kprintf("tests complete, shutting down.\n");
    sbi_shutdown();
}
