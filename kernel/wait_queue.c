/*
 * wait_queue.c — Wait queue implementation for bobchouOS.
 *
 * Provides targeted sleep/wakeup. Each event owns a list of waiters;
 * wakeup touches only that list — O(waiters) not O(all processes).
 *
 * See Lecture 5-2, Part 4.
 */

#include "wait_queue.h"
#include "proc.h"
#include "kprintf.h"

/*
 * wq_sleep — sleep on a wait queue, atomically releasing lk.
 *
 * Precondition: caller holds lk (the condition lock, acquired via irqsave).
 * Postcondition: lk is re-acquired before returning.
 *
 * Caller MUST call this inside a loop that re-checks the condition after
 * waking — never assume the wakeup means the condition is true. Wakeups
 * can be spurious (another consumer reaped the event first). Typical usage:
 *
 *   for (;;) {
 *       if (condition_met) { ... return; }  // early return
 *       wq_sleep(&wq, &lk);
 *       // loops back — re-checks condition
 *   }
 *
 * Lock ordering: caller holds lk → acquire wq->lock → acquire p->lock.
 * Release order: lk (lost wakeup solved), wq->lock (queue consistent),
 * p->lock crosses into scheduler (golden rule).
 */
void
wq_sleep(struct wait_queue *wq, struct spinlock *lk) {
    struct proc *p = this_proc();

    spin_lock(&wq->lock);
    spin_lock(&p->lock);

    list_add_tail(&p->wait_link, &wq->head);
    p->state = PROC_SLEEPING;

    spin_unlock(lk);        /* condition lock released — lost wakeup solved */
    spin_unlock(&wq->lock); /* done modifying queue */
    sched();                /* p->lock crosses boundary, released by scheduler */

    spin_lock(lk); /* re-acquire condition lock before returning */
}

/*
 * wq_wake_one — wake the first waiter on the queue.
 *
 * Returns 1 if a process was woken, 0 if queue was empty.
 * The waker removes the process from the queue (under wq->lock),
 * sets it RUNNABLE, and adds it to the run queue.
 */
int
wq_wake_one(struct wait_queue *wq) {
    unsigned long irq;
    spin_lock_irqsave(&wq->lock, &irq);

    if (list_empty(&wq->head)) {
        spin_unlock_irqrestore(&wq->lock, irq);
        return 0;
    }

    struct proc *p = list_first_entry(&wq->head, struct proc, wait_link);
    list_del(&p->wait_link);

    spin_lock(&p->lock);
    p->state = PROC_RUNNABLE;
    spin_unlock(&p->lock);

    run_queue_add(p);

    spin_unlock_irqrestore(&wq->lock, irq);
    return 1;
}

/*
 * wq_wake_all — wake all waiters on the queue.
 */
void
wq_wake_all(struct wait_queue *wq) {
    unsigned long irq;
    spin_lock_irqsave(&wq->lock, &irq);

    while (!list_empty(&wq->head)) {
        struct proc *p = list_first_entry(&wq->head, struct proc, wait_link);
        list_del(&p->wait_link);

        spin_lock(&p->lock);
        p->state = PROC_RUNNABLE;
        spin_unlock(&p->lock);

        run_queue_add(p);
    }

    spin_unlock_irqrestore(&wq->lock, irq);
}
