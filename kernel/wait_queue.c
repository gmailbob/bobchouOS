/*
 * wait_queue.c — Wait queue implementation for bobchouOS.
 *
 * See Lecture 5-2, Part 4.
 */

#include "wait_queue.h"
#include "proc.h"
#include "kprintf.h"

/*
 * wq_init — initialize a wait queue.
 *
 * TODO:
 * - spin_init the embedded lock
 * - INIT_LIST_HEAD the embedded head
 */
void
wq_init(struct wait_queue *wq, const char *name) {
    (void)wq;
    (void)name;
    panic("wq_init: not implemented");
}

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
 *
 * TODO:
 * - Get current proc via this_proc()
 * - Acquire wq->lock (plain — interrupts already off from caller's irqsave)
 * - Acquire p->lock (plain)
 * - list_add_tail(&p->wait_link, &wq->head)
 * - p->state = PROC_SLEEPING
 * - spin_unlock(lk) — release condition lock, lost wakeup solved
 * - spin_unlock(&wq->lock) — done modifying queue
 * - sched() — switch away, p->lock released by scheduler
 * - On wakeup: spin_lock(lk) — re-acquire condition lock before return
 */
void
wq_sleep(struct wait_queue *wq, struct spinlock *lk) {
    (void)wq;
    (void)lk;
    panic("wq_sleep: not implemented");
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
    (void)wq;
    panic("wq_wake_one: not implemented");
}

/*
 * wq_wake_all — wake all waiters on the queue.
 *
 * TODO:
 * - Same logic as wq_wake_one but loop until queue is empty
 */
void
wq_wake_all(struct wait_queue *wq) {
    (void)wq;
    panic("wq_wake_all: not implemented");
}
