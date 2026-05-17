/*
 * wait_queue.c — Wait queue implementation for bobchouOS.
 *
 * See Lecture 5-2, Part 4.
 */

#include "wait_queue.h"
#include "proc.h"
#include "kprintf.h"

/*
 * wq_sleep — sleep on a wait queue, atomically releasing lk.
 *
 * Solves the lost wakeup problem: adds self to queue and marks
 * SLEEPING before releasing the condition lock.
 * Re-acquires lk before returning (POSIX condvar pattern).
 *
 * Lock ordering: caller holds lk → acquire wq->lock → acquire p->lock.
 * Release: lk first (lost wakeup solved), wq->lock second,
 * p->lock crosses into scheduler (golden rule).
 */
void
wq_sleep(struct wait_queue *wq, struct spinlock *lk) {
    struct proc *p = this_proc();

    spin_lock(&wq->lock);
    spin_lock(&p->lock);
    list_add_tail(&p->wait_link, &wq->head);
    p->state = PROC_SLEEPING;

    // caller holds lock until here, lost wakeup solved
    // at the same time we must free the lock before give up cpu so the producer can proceed
    spin_unlock(lk);
    spin_unlock(&wq->lock); // done modifying queue
    sched();                // p->lock cross boundry will be released by scheduler

    spin_lock(lk); // POSIX condvar pattern: re-acquires lk
}

/*
 * wq_wake_one — wake the first waiter on the queue.
 *
 * Returns 1 if a process was woken, 0 if queue was empty.
 * The waker removes the process from the queue (under wq->lock).
 *
 * TODO:
 * - Acquire wq->lock
 * - If queue empty (list_empty), unlock and return 0
 * - list_first_entry to get the proc, list_del(&p->wait_link)
 * - Release wq->lock
 * - Acquire p->lock, set state = PROC_RUNNABLE
 * - Acquire run_queue_lock, list_add_tail(&p->run_list, &run_queue)
 * - Release run_queue_lock
 * - Release p->lock
 * - Return 1
 */
int
wq_wake_one(struct wait_queue *wq) {
    uint64 irq;
    spin_lock_irqsave(&wq->lock, &irq);

    if (list_empty(&wq->head)) {
        spin_unlock_irqrestore(&wq->lock, irq);
        return 0;
    }

    struct proc *p = list_first_entry(&wq->head, struct proc, wait_link);
    list_del(&p->wait_link);

    spin_lock(&p->lock);
    p->state = PROC_RUNNABLE;
    run_queue_add(p);
    spin_lock(&p->lock);

    spin_unlock_irqrestore(&wq->lock, irq);
    return 1;
}

/*
 * wq_wake_all — wake all waiters on the queue.
 */
void
wq_wake_all(struct wait_queue *wq) {
    uint64 irq;
    spin_lock_irqsave(&wq->lock, &irq);

    while (!list_empty(&wq->head)) {
        struct proc *p = list_first_entry(&wq->head, struct proc, wait_link);
        list_del(&p->wait_link);
        spin_lock(&p->lock);
        p->state = PROC_RUNNABLE;
        run_queue_add(p);
        spin_lock(&p->lock);
    }

    spin_unlock_irqrestore(&wq->lock, irq);
}
