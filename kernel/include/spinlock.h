/*
 * spinlock.h — Spinlock interface for bobchouOS.
 *
 * Two variants: irqsave (saves/restores interrupt state) for outermost
 * locks, and plain (no interrupt change) for inner locks when interrupts
 * are provably off.
 *
 * See Lecture 5-2, Part 3.
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

struct spinlock {
    int locked;
    const char *name;
};

void spin_init(struct spinlock *lk, const char *name);
void spin_lock_irqsave(struct spinlock *lk, unsigned long *flags);
void spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags);
void spin_lock(struct spinlock *lk);
void spin_unlock(struct spinlock *lk);

/* Returns 1 if lk is currently held (for debug assertions). */
int spin_holding(struct spinlock *lk);

#endif /* SPINLOCK_H */
