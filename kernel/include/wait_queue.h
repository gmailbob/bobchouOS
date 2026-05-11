/*
 * wait_queue.h — Wait queue interface for bobchouOS.
 *
 * Provides targeted sleep/wakeup using per-event linked lists.
 * Replaces xv6's void* channel scan with O(waiters) wakeup.
 *
 * See Lecture 5-2, Part 4.
 */

#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include "list.h"
#include "spinlock.h"

struct wait_queue {
    struct spinlock lock;
    struct list_head head;
};

void wq_init(struct wait_queue *wq, const char *name);
void wq_sleep(struct wait_queue *wq, struct spinlock *lk);
int wq_wake_one(struct wait_queue *wq);
void wq_wake_all(struct wait_queue *wq);

#endif /* WAIT_QUEUE_H */
