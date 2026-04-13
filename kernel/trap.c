/*
 * trap.c — S-mode trap dispatcher for bobchouOS.
 *
 * Called from kernel_vec (kernel_vec.S) after registers are saved.
 * Reads scause to determine what happened and dispatches accordingly.
 * Currently handles timer ticks (Round 2-3); exceptions are fatal.
 * Will grow to include usertrap() and devintr() in later phases.
 *
 * Refer to Lecture 2-2 (Part 7) and Lecture 2-3 (Part 6).
 */

#include "riscv.h"
#include "kprintf.h"

void
kernel_trap(void) {
    uint64 sepc_val = csrr(sepc);
    uint64 scause_val = csrr(scause);
    uint64 stval_val = csrr(stval);
    uint64 sstatus_val = csrr(sstatus);

    /* Sanity checks: */
    if (!(sstatus_val & SSTATUS_SPP))
        kprintf("sstatus.SPP not yet set?");
    if (sstatus_val & SSTATUS_SIE)
        kprintf("sstatus.SIE still set?");

    /* Check scause bit 63 to distinguish interrupts vs exceptions. */
    if (scause_val & SCAUSE_INTERRUPT) {
        /* Interrupt — we don't handle any yet (timer comes in Round 2-3). */
        panic("kernel_trap: unexpected interrupt code=%d", (int)(scause_val & 0xff));
    } else {
        /* Exception — always fatal in kernel mode. */
        panic("kernel_trap: exception scause=%p sepc=%p stval=%p", (void *)scause_val,
              (void *)sepc_val, (void *)stval_val);
    }
}
