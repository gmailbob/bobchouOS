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
    int locked; /* 0=free, 1=held. int because amoswap.w is 32-bit */
    const char *name;
};

static inline void
spin_init(struct spinlock *lk, const char *name) {
    lk->locked = 0;
    lk->name = name;
}

/*
 * Returns 1 if lk is currently held. For debug assertions only —
 * call it when you believe you hold the lock; if it returns 0, you
 * have a bug. Just peeks the int value (no atomic needed for a read).
 * Linux equivalent: lockdep_assert_held().
 */
static inline int
spin_holding(struct spinlock *lk) {
    return lk->locked;
}

void spin_lock_irqsave(struct spinlock *lk, unsigned long *flags);
void spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags);
void spin_lock(struct spinlock *lk);
void spin_unlock(struct spinlock *lk);

#endif /* SPINLOCK_H */
