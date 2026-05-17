/*
 * test_wait_queue.c — Tests for wait queue implementation.
 *
 * Note: full sleep/wakeup tests (with sched) require running processes
 * and are tested via the init/worker scenario in `make run`. These unit
 * tests cover the data structure operations and wakeup logic that can
 * be tested without scheduling.
 */

#include "test/test.h"
#include "wait_queue.h"
#include "spinlock.h"
#include "proc.h"
#include "list.h"
#include "string.h"

void
test_wait_queue(void) {
    kprintf("[test_wait_queue]\n");

    /* wq_init creates empty queue */
    struct wait_queue wq;
    wq_init(&wq, "test_wq");
    TEST_ASSERT(list_empty(&wq.head), "wq_init: queue is empty");
    TEST_ASSERT(wq.lock.locked == 0, "wq_init: lock is unlocked");

    /* wq_wake_one on empty queue returns 0 */
    TEST_ASSERT(wq_wake_one(&wq) == 0, "wq_wake_one: empty queue returns 0");

    /* wq_wake_all on empty queue does nothing (no crash) */
    wq_wake_all(&wq);
    TEST_ASSERT(list_empty(&wq.head), "wq_wake_all: empty queue still empty");

    /* Simulate adding a proc to the queue (bypassing wq_sleep which
     * needs scheduling). This tests wq_wake_one's removal logic. */
    struct proc fake_proc;
    memset(&fake_proc, 0, sizeof(fake_proc));
    spin_init(&fake_proc.lock, "fake");
    INIT_LIST_HEAD(&fake_proc.wait_link);
    INIT_LIST_HEAD(&fake_proc.run_list);
    fake_proc.state = PROC_SLEEPING;

    /* Manually add to queue (simulating what wq_sleep does) */
    list_add_tail(&fake_proc.wait_link, &wq.head);
    TEST_ASSERT(!list_empty(&wq.head), "manually added proc: queue not empty");

    /* wq_wake_one should remove it and set RUNNABLE */
    int woke = wq_wake_one(&wq);
    TEST_ASSERT(woke == 1, "wq_wake_one: returns 1 when proc found");
    TEST_ASSERT(fake_proc.state == PROC_RUNNABLE, "wq_wake_one: sets state RUNNABLE");
    TEST_ASSERT(list_empty(&wq.head), "wq_wake_one: queue empty after wake");

    /* Test wq_wake_all with multiple entries */
    struct proc fake2, fake3;
    memset(&fake2, 0, sizeof(fake2));
    memset(&fake3, 0, sizeof(fake3));
    spin_init(&fake2.lock, "fake2");
    spin_init(&fake3.lock, "fake3");
    INIT_LIST_HEAD(&fake2.wait_link);
    INIT_LIST_HEAD(&fake2.run_list);
    INIT_LIST_HEAD(&fake3.wait_link);
    INIT_LIST_HEAD(&fake3.run_list);
    fake2.state = PROC_SLEEPING;
    fake3.state = PROC_SLEEPING;

    list_add_tail(&fake2.wait_link, &wq.head);
    list_add_tail(&fake3.wait_link, &wq.head);
    TEST_ASSERT(!list_empty(&wq.head), "two procs added: queue not empty");

    wq_wake_all(&wq);
    TEST_ASSERT(fake2.state == PROC_RUNNABLE, "wq_wake_all: proc2 RUNNABLE");
    TEST_ASSERT(fake3.state == PROC_RUNNABLE, "wq_wake_all: proc3 RUNNABLE");
    TEST_ASSERT(list_empty(&wq.head), "wq_wake_all: queue empty after");
}
