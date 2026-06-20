/*
 * test_sleeplock.c — Integration test for the sleep-lock.
 *
 * Needs the scheduler: a contended sleep_lock sleeps the caller, so a
 * cooperating thread is required to exercise the blocking path.
 *
 * See Lecture 7-2, Part 3.
 */

#include "test/test.h"
#include "sleeplock.h"
#include "proc.h"

/* Shared between the main test and the contender thread. */
static struct sleeplock test_sl;
static volatile int contender_ran;  /* contender reached sleep_lock */
static volatile int contender_got;  /* contender acquired the lock */

/*
 * contender_thread — try to acquire a lock the main thread already holds.
 * Must block (sleep) inside sleep_lock until main releases, then proceed.
 */
static void
contender_thread(void) {
    kthread_start();

    contender_ran = 1;
    sleep_lock(&test_sl); /* blocks: main holds it */
    contender_got = 1;    /* only reached after main releases */
    sleep_unlock(&test_sl);
    proc_exit(0);
}

void
test_sleeplock(void) {
    kprintf("[test_sleeplock]\n");

    /* --- Basic state: init / acquire / release (single thread) --- */

    sleep_init(&test_sl, "test_sl");
    TEST_ASSERT(test_sl.locked == 0, "sleep_init: not held");
    TEST_ASSERT(sleep_holding(&test_sl) == 0, "sleep_init: sleep_holding == 0");

    sleep_lock(&test_sl);
    TEST_ASSERT(test_sl.locked == 1, "sleep_lock: locked == 1");
    TEST_ASSERT(sleep_holding(&test_sl) == 1, "sleep_lock: current proc holds it");

    sleep_unlock(&test_sl);
    TEST_ASSERT(test_sl.locked == 0, "sleep_unlock: locked == 0");
    TEST_ASSERT(sleep_holding(&test_sl) == 0, "sleep_unlock: no longer held");

    /* --- Contention: a waiter must SLEEP, then acquire on release --- */

    sleep_init(&test_sl, "test_sl_live");
    contender_ran = 0;
    contender_got = 0;

    /* Main thread takes the lock first. */
    sleep_lock(&test_sl);

    struct proc *contender = proc_create_kernel(contender_thread, "sl_contend");
    TEST_ASSERT(contender != 0, "contender process created");

    /* Let it run and block inside sleep_lock (we still hold the lock). */
    for (int i = 0; i < 100; i++) {
        yield();
        spin_lock(&contender->lock);
        int asleep = (contender->state == PROC_SLEEPING);
        spin_unlock(&contender->lock);
        if (contender_ran && asleep)
            break;
    }

    TEST_ASSERT(contender_ran == 1, "contender entered sleep_lock");
    spin_lock(&contender->lock);
    TEST_ASSERT(contender->state == PROC_SLEEPING, "contender is SLEEPING (blocked on lock)");
    spin_unlock(&contender->lock);
    TEST_ASSERT(contender_got == 0, "contender has NOT acquired while main holds it");

    /* Release: the contender should wake, acquire, and run to exit. */
    sleep_unlock(&test_sl);
    for (int i = 0; i < 100; i++) {
        yield();
        if (contender_got)
            break;
    }
    TEST_ASSERT(contender_got == 1, "contender acquired after main released");

    /* Let it finish exiting. */
    for (int i = 0; i < 100; i++) {
        yield();
        spin_lock(&contender->lock);
        int done = (contender->state == PROC_ZOMBIE);
        spin_unlock(&contender->lock);
        if (done)
            break;
    }
    spin_lock(&contender->lock);
    TEST_ASSERT(contender->state == PROC_ZOMBIE, "contender exited");
    spin_unlock(&contender->lock);
}
