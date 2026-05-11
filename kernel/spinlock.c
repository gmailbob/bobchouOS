/*
 * spinlock.c — Spinlock implementation for bobchouOS.
 *
 * Uses RISC-V amoswap for atomic acquire, fence for memory ordering.
 * See Lecture 5-2, Part 3.
 */

#include "spinlock.h"
#include "riscv.h"
#include "kprintf.h"

/* --- Spinlock implementation --- */

void
spin_init(struct spinlock *lk, const char *name) {
    /* TODO: initialize locked = 0, name = name */
    (void)lk;
    (void)name;
}

/*
 * spin_lock_irqsave — acquire lock, saving and disabling interrupts.
 *
 * TODO:
 * - Save current SIE state into *flags via intr_get()
 * - Disable interrupts via intr_off()
 * - Spin on __sync_lock_test_and_set(&lk->locked, 1) until it returns 0
 * - __sync_synchronize() — full fence after acquire
 */
void
spin_lock_irqsave(struct spinlock *lk, unsigned long *flags) {
    (void)lk;
    (void)flags;
    panic("spin_lock_irqsave: not implemented");
}

/*
 * spin_unlock_irqrestore — release lock, restoring interrupt state.
 *
 * TODO:
 * - __sync_synchronize() — full fence before release
 * - __sync_lock_release(&lk->locked) — atomic store 0
 * - If flags indicates interrupts were on, call intr_on()
 */
void
spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags) {
    (void)lk;
    (void)flags;
    panic("spin_unlock_irqrestore: not implemented");
}

/*
 * spin_lock — acquire lock without touching interrupt state.
 * Use only when interrupts are provably already off.
 *
 * TODO:
 * - Spin on __sync_lock_test_and_set(&lk->locked, 1) until it returns 0
 * - __sync_synchronize() — full fence after acquire
 */
void
spin_lock(struct spinlock *lk) {
    (void)lk;
    panic("spin_lock: not implemented");
}

/*
 * spin_unlock — release lock without touching interrupt state.
 *
 * TODO:
 * - __sync_synchronize() — full fence before release
 * - __sync_lock_release(&lk->locked) — atomic store 0
 */
void
spin_unlock(struct spinlock *lk) {
    (void)lk;
    panic("spin_unlock: not implemented");
}

/*
 * spin_holding — returns 1 if lock is currently held.
 * For debug assertions only.
 */
int
spin_holding(struct spinlock *lk) {
    /* TODO: return lk->locked */
    (void)lk;
    return 0;
}
