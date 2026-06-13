/*
 * test_spinlock_contention.c — Integration test: mutual exclusion under
 * contention between two kernel threads.
 *
 * Verifies that a spinlock actually provides mutual exclusion by having
 * two threads increment a shared counter under the lock. Without proper
 * locking, the final count would be less than expected (lost updates).
 *
 * Note: on a single-hart system, contention only occurs through
 * preemption (timer interrupt during the critical section). The test
 * still validates correctness — the lock prevents the preempted thread
 * from being re-entered before unlock.
 */

#include "test/test.h"
#include "spinlock.h"
#include "proc.h"

#define ITERATIONS 100

static struct spinlock counter_lock;
static volatile int shared_counter;

static void
incrementer_thread(void) {
    kthread_start();
    for (int i = 0; i < ITERATIONS; i++) {
        unsigned long irq;
        spin_lock_irqsave(&counter_lock, &irq);
        shared_counter++;
        spin_unlock_irqrestore(&counter_lock, irq);
        yield();
    }
    proc_exit(0);
}

void
test_spinlock_contention(void) {
    kprintf("[test_spinlock_contention]\n");

    spin_init(&counter_lock, "cnt_lk");
    shared_counter = 0;

    struct proc *t1 = proc_create_kernel(incrementer_thread, "inc1");
    struct proc *t2 = proc_create_kernel(incrementer_thread, "inc2");
    TEST_ASSERT(t1 != 0 && t2 != 0, "incrementer threads created");

    /* Let them both run to completion */
    for (int i = 0; i < ITERATIONS * 4; i++)
        yield();

    TEST_ASSERT(shared_counter == ITERATIONS * 2, "counter == 200: mutual exclusion preserved");
}
