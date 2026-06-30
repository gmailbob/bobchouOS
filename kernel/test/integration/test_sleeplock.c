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
static volatile int contender_done; /* contender released + about to exit */

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
    contender_done = 1; /* publish in OUR storage before exit (see note below) */
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
    contender_done = 0;

    /* Main thread takes the lock first. */
    sleep_lock(&test_sl);

    struct proc *contender = proc_create_kernel(contender_thread, "sl_contend");
    TEST_ASSERT(contender != 0, "contender process created");

    /* Let it run and block inside sleep_lock (we still hold the lock).
     * Inspecting contender->state here is safe: it's blocked, so the struct
     * proc is alive (contrast the exit wait below, where it may be freed). */
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

    /* Wait via the contender_done flag, NOT contender->state: after
     * proc_exit() the contender is reaped (free_proc frees its struct proc),
     * so the pointer dangles — reading contender->lock.locked yields garbage
     * and spin_lock() would spin forever on it. The flag lives in our data
     * segment and survives the reap. */
    for (int i = 0; i < 100; i++) {
        yield();
        if (contender_done)
            break;
    }
    TEST_ASSERT(contender_done == 1, "contender released and exited");
    (void)contender; /* do NOT dereference after exit — struct may be freed */
}
