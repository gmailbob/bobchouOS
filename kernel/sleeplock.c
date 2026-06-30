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
 */
void
sleep_init(struct sleeplock *sl, const char *name) {
    sl->locked = 0;
    wq_init(&sl->wq, name);
    spin_init(&sl->lk, name);
    sl->pid = 0;
}

/*
 * sleep_lock — acquire, sleeping while another holder owns it.
 *
 * The lost-wakeup-safe pattern: take the internal spinlock, then loop
 * "while held, wq_sleep" — re-checking `locked` after every wake. Once
 * the loop exits, claim ownership and release the internal spinlock.
 *
 * Precondition: caller holds NO spinlock (this may sleep).
 */
void
sleep_lock(struct sleeplock *sl) {
    unsigned long irq;
    spin_lock_irqsave(&sl->lk, &irq);

    while (sl->locked)
        wq_sleep(&sl->wq, &sl->lk);
    sl->locked = 1;
    sl->pid = this_proc()->pid;

    spin_unlock_irqrestore(&sl->lk, irq);
}

/*
 * sleep_unlock — release and wake one waiter.
 *
 * Precondition: caller holds the lock.
 */
void
sleep_unlock(struct sleeplock *sl) {
    unsigned long irq;
    spin_lock_irqsave(&sl->lk, &irq);
    sl->locked = 0;
    sl->pid = 0;
    wq_wake_one(&sl->wq);
    spin_unlock_irqrestore(&sl->lk, irq);
}

/*
 * sleep_holding — 1 if the current process holds this lock.
 *
 * Reads `locked` and `pid` under the internal spinlock. Used by
 * assertions (e.g. bwrite/brelse check the caller owns the buf).
 */
int
sleep_holding(struct sleeplock *sl) {
    spin_lock(&sl->lk);
    int holding = sl->locked && sl->pid == this_proc()->pid;
    spin_unlock(&sl->lk);
    return holding;
}
