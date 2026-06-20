/*
 * sleeplock.h — Sleep-lock (kernel mutex) interface for bobchouOS.
 *
 * A sleep-lock is a mutual-exclusion lock whose waiters *sleep* instead
 * of spinning, so it may be held for a long time — across disk I/O, in
 * particular. It is the general kernel mutex: the buffer cache (7-2)
 * embeds one per buf; the inode layer (7-5) and others reuse the same
 * type.
 *
 * Built on the spinlock (5-2) + wait_queue (5-2): a short internal
 * spinlock guards the `locked` flag for the few nanoseconds it takes to
 * test-and-set it; contenders park on the wait queue. Contrast with
 * spinlock, whose waiters busy-wait with interrupts disabled.
 *
 * Naming mirrors spin_*: sleep_lock / sleep_unlock / sleep_init /
 * sleep_holding.
 *
 * See Lecture 7-2, Part 3.
 */

#ifndef SLEEPLOCK_H
#define SLEEPLOCK_H

#include "spinlock.h"
#include "wait_queue.h"

/*
 * struct sleeplock — a mutex you may hold across I/O.
 *
 * `locked` is the ownership flag: 1 for the entire span a holder owns
 * the lock (sleep_lock → sleep_unlock), potentially milliseconds. `lk`
 * is a short internal spinlock that makes reading/updating `locked`
 * atomic — held only for nanoseconds, free for the rest of the time.
 * (Two distinct "locked"s, two durations — see Lecture 7-2, Part 3.)
 */
struct sleeplock {
    int locked;           /* is the lock held? (ownership flag) */
    struct wait_queue wq; /* contenders sleep here */
    struct spinlock lk;   /* protects `locked` — held only nanoseconds */
    int pid;              /* holder's pid (debug / sleep_holding) */
};

/* Initialize a sleep-lock: unheld, empty wait queue. */
void sleep_init(struct sleeplock *sl, const char *name);

/* Acquire. Sleeps (yielding the CPU) while another holder owns it;
 * returns with the lock held. Must NOT be called holding a spinlock. */
void sleep_lock(struct sleeplock *sl);

/* Release. Wakes one waiter (if any). Caller must hold the lock. */
void sleep_unlock(struct sleeplock *sl);

/* Debug predicate: 1 if the *current* process holds this lock. */
int sleep_holding(struct sleeplock *sl);

#endif /* SLEEPLOCK_H */
