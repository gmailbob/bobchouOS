/*
 * spinlock.c — Spinlock implementation for bobchouOS.
 *
 * Uses RISC-V amoswap for atomic acquire, fence for memory ordering.
 * See Lecture 5-2, Part 3.
 */

#include "spinlock.h"
#include "riscv.h"
#include "kprintf.h"

/*
 * spin_lock_irqsave — acquire lock, saving and disabling interrupts.
 */
void
spin_lock_irqsave(struct spinlock *lk, unsigned long *flags) {
    *flags = intr_get();
    intr_off();

    // always performs the swap unconditionally and returns the old value
    while (__sync_lock_test_and_set(&lk->locked, 1))
        ;
    __sync_synchronize();
}

/*
 * spin_unlock_irqrestore — release lock, restoring interrupt state.
 */
void
spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags) {
    __sync_synchronize();
    __sync_lock_release(&lk->locked);
    if (flags)
        intr_on();
}

/*
 * spin_lock — acquire lock without touching interrupt state.
 * Use only when interrupts are provably already off.
 */
void
spin_lock(struct spinlock *lk) {
    while (__sync_lock_test_and_set(&lk->locked, 1))
        ;
    __sync_synchronize();
}

/*
 * spin_unlock — release lock without touching interrupt state.
 */
void
spin_unlock(struct spinlock *lk) {
    __sync_synchronize();
    __sync_lock_release(&lk->locked);
}
