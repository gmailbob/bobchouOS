/*
 * sleeplock.c — Sleep-lock (kernel mutex) implementation.
 *
 * The canonical condition-loop from Lecture 5-2, packaged as a reusable
 * lock: a `locked` flag guarded by a short spinlock, with contenders
 * parked on a wait queue. See Lecture 7-2, Part 3.
 */

#include "sleeplock.h"
#include "proc.h"
#include "kprintf.h"

/*
 * sleep_init — initialize a sleep-lock to the unheld state.
 *
 * TODO(you): init the internal spinlock (spin_init) and the wait queue
 * (wq_init), clear `locked`, and zero `pid`. Pass `name` down to the
 * sub-locks for debugging.
 */
void
sleep_init(struct sleeplock *sl, const char *name) {
    /* TODO */
}

/*
 * sleep_lock — acquire, sleeping while another holder owns it.
 *
 * The lost-wakeup-safe pattern: take the internal spinlock, then loop
 * "while held, wq_sleep" — re-checking `locked` after every wake. Once
 * the loop exits, claim ownership and release the internal spinlock.
 *
 * Precondition: caller holds NO spinlock (this may sleep).
 *
 * TODO(you):
 *   - spin_lock(&sl->lk)
 *   - while (sl->locked) wq_sleep(&sl->wq, &sl->lk)
 *   - set locked = 1, record pid = this_proc()->pid
 *   - spin_unlock(&sl->lk)
 */
void
sleep_lock(struct sleeplock *sl) {
    /* TODO */
}

/*
 * sleep_unlock — release and wake one waiter.
 *
 * Precondition: caller holds the lock.
 *
 * TODO(you):
 *   - spin_lock(&sl->lk)
 *   - clear locked, clear pid
 *   - wq_wake_one(&sl->wq)   (one waiter — only one can take the lock)
 *   - spin_unlock(&sl->lk)
 */
void
sleep_unlock(struct sleeplock *sl) {
    /* TODO */
}

/*
 * sleep_holding — 1 if the current process holds this lock.
 *
 * Reads `locked` and `pid` under the internal spinlock. Used by
 * assertions (e.g. bwrite/brelse check the caller owns the buf).
 *
 * TODO(you): under sl->lk, return (locked && pid == this_proc()->pid).
 */
int
sleep_holding(struct sleeplock *sl) {
    /* TODO */
    return 0;
}
