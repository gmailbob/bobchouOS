/*
 * test_spinlock.c — Tests for spinlock implementation.
 */

#include "test/test.h"
#include "spinlock.h"
#include "riscv.h"

void
test_spinlock(void) {
    kprintf("[test_spinlock]\n");

    struct spinlock lk;

    /* spin_init */
    spin_init(&lk, "test_lock");
    TEST_ASSERT(lk.locked == 0, "spin_init sets locked=0");
    TEST_ASSERT(lk.name != 0, "spin_init sets name");

    /* spin_holding */
    TEST_ASSERT(spin_holding(&lk) == 0, "spin_holding: not held initially");

    /* spin_lock / spin_unlock (plain variant) */
    uint64 old_sstatus = csrr(sstatus);
    csrw(sstatus, old_sstatus & ~SSTATUS_SIE); /* ensure interrupts off */

    spin_lock(&lk);
    TEST_ASSERT(lk.locked == 1, "spin_lock sets locked=1");
    TEST_ASSERT(spin_holding(&lk) == 1, "spin_holding: held after lock");

    spin_unlock(&lk);
    TEST_ASSERT(lk.locked == 0, "spin_unlock sets locked=0");
    TEST_ASSERT(spin_holding(&lk) == 0, "spin_holding: not held after unlock");

    csrw(sstatus, old_sstatus); /* restore */

    /* spin_lock_irqsave disables interrupts */
    csrw(sstatus, csrr(sstatus) | SSTATUS_SIE); /* turn on SIE */
    TEST_ASSERT((csrr(sstatus) & SSTATUS_SIE) != 0, "SIE on before irqsave");

    unsigned long flags;
    spin_lock_irqsave(&lk, &flags);
    TEST_ASSERT((csrr(sstatus) & SSTATUS_SIE) == 0, "irqsave disables SIE");
    TEST_ASSERT(flags != 0, "irqsave saves old SIE=1 in flags");
    TEST_ASSERT(lk.locked == 1, "irqsave acquires lock");

    spin_unlock_irqrestore(&lk, flags);
    TEST_ASSERT(lk.locked == 0, "irqrestore releases lock");
    TEST_ASSERT((csrr(sstatus) & SSTATUS_SIE) != 0, "irqrestore restores SIE=1");

    /* irqsave when interrupts already off */
    csrw(sstatus, csrr(sstatus) & ~SSTATUS_SIE); /* turn off SIE */
    unsigned long flags2;
    spin_lock_irqsave(&lk, &flags2);
    TEST_ASSERT(flags2 == 0, "irqsave saves SIE=0 when already off");

    spin_unlock_irqrestore(&lk, flags2);
    TEST_ASSERT((csrr(sstatus) & SSTATUS_SIE) == 0, "irqrestore keeps SIE=0 when saved 0");

    /* nested locks: inner unlock doesn't re-enable interrupts */
    csrw(sstatus, csrr(sstatus) | SSTATUS_SIE); /* start with SIE on */
    struct spinlock lk2;
    spin_init(&lk2, "test_lock2");

    unsigned long f1, f2;
    spin_lock_irqsave(&lk, &f1);  /* outer: saves SIE=1, disables */
    spin_lock_irqsave(&lk2, &f2); /* inner: saves SIE=0 */
    TEST_ASSERT((csrr(sstatus) & SSTATUS_SIE) == 0, "nested: SIE off");

    spin_unlock_irqrestore(&lk2, f2); /* inner: f2=0, stays off */
    TEST_ASSERT((csrr(sstatus) & SSTATUS_SIE) == 0, "nested: inner unlock keeps SIE off");

    spin_unlock_irqrestore(&lk, f1); /* outer: f1=1, re-enables */
    TEST_ASSERT((csrr(sstatus) & SSTATUS_SIE) != 0, "nested: outer unlock restores SIE on");
}
