/*
 * test_wait_queue.c — Integration tests for wait queue sleep/wakeup.
 *
 * Now runs with the scheduler live, so we can test real wq_sleep and
 * wq_wake_one/wq_wake_all with actual process context switches.
 */

#include "test/test.h"
#include "wait_queue.h"
#include "spinlock.h"
#include "proc.h"
#include "list.h"
#include "string.h"

static struct wait_queue test_wq;
static struct spinlock test_lk;
static volatile int shared_flag;

static void
sleeper_thread(void) {
    kthread_start();

    unsigned long irq;
    spin_lock_irqsave(&test_lk, &irq);
    while (!shared_flag)
        wq_sleep(&test_wq, &test_lk);
    spin_unlock_irqrestore(&test_lk, irq);
    proc_exit(0);
}

static volatile int wake_all_count;

static void
wake_all_thread(void) {
    kthread_start();

    unsigned long irq;
    spin_lock_irqsave(&test_lk, &irq);
    while (!shared_flag)
        wq_sleep(&test_wq, &test_lk);
    __sync_fetch_and_add(&wake_all_count, 1);
    spin_unlock_irqrestore(&test_lk, irq);
    proc_exit(0);
}

void
test_wait_queue(void) {
    kprintf("[test_wait_queue]\n");

    /* --- Data structure tests (no scheduling needed) --- */

    struct wait_queue wq;
    wq_init(&wq, "test_wq");
    TEST_ASSERT(list_empty(&wq.head), "wq_init: queue is empty");
    TEST_ASSERT(wq.lock.locked == 0, "wq_init: lock is unlocked");

    TEST_ASSERT(wq_wake_one(&wq) == 0, "wq_wake_one: empty queue returns 0");

    wq_wake_all(&wq);
    TEST_ASSERT(list_empty(&wq.head), "wq_wake_all: empty queue still empty");

    /* --- Real sleep/wake: single thread --- */

    wq_init(&test_wq, "test_wq_live");
    spin_init(&test_lk, "test_lk");
    shared_flag = 0;

    struct proc *sleeper = proc_create_kernel(sleeper_thread, "sleeper");
    TEST_ASSERT(sleeper != 0, "sleeper process created");

    /* Let the sleeper run and block on the wait queue */
    for (int i = 0; i < 100; i++) {
        yield();
        spin_lock(&sleeper->lock);
        int asleep = (sleeper->state == PROC_SLEEPING);
        spin_unlock(&sleeper->lock);
        if (asleep)
            break;
    }

    spin_lock(&sleeper->lock);
    TEST_ASSERT(sleeper->state == PROC_SLEEPING, "sleeper is SLEEPING after yield");
    spin_unlock(&sleeper->lock);

    /* Wake it: set condition + wake */
    unsigned long irq;
    spin_lock_irqsave(&test_lk, &irq);
    shared_flag = 1;
    wq_wake_one(&test_wq);
    spin_unlock_irqrestore(&test_lk, irq);

    /* Let it run and exit */
    for (int i = 0; i < 100; i++) {
        yield();
        spin_lock(&sleeper->lock);
        int done = (sleeper->state == PROC_ZOMBIE);
        spin_unlock(&sleeper->lock);
        if (done)
            break;
    }

    spin_lock(&sleeper->lock);
    TEST_ASSERT(sleeper->state == PROC_ZOMBIE, "sleeper exited after wake");
    TEST_ASSERT(sleeper->exit_status == 0, "sleeper exit status == 0");
    spin_unlock(&sleeper->lock);

    /* --- wq_wake_all: multiple threads --- */

    shared_flag = 0;
    wake_all_count = 0;

    struct proc *w1 = proc_create_kernel(wake_all_thread, "waker1");
    struct proc *w2 = proc_create_kernel(wake_all_thread, "waker2");
    TEST_ASSERT(w1 != 0 && w2 != 0, "wake_all threads created");

    /* Let both sleep */
    for (int i = 0; i < 100; i++)
        yield();

    /* Wake all */
    spin_lock_irqsave(&test_lk, &irq);
    shared_flag = 1;
    wq_wake_all(&test_wq);
    spin_unlock_irqrestore(&test_lk, irq);

    /* Let them run and exit */
    for (int i = 0; i < 100; i++) {
        yield();
        if (wake_all_count == 2)
            break;
    }

    TEST_ASSERT(wake_all_count == 2, "wq_wake_all: both threads woke");
}
