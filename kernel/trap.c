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

/* Timer tick counter. Volatile because it is modified in interrupt
 * context (kernel_trap) and read from normal context (kmain loop). */
volatile uint64 ticks = 0;

void
kernel_trap(void) {
    uint64 sepc_val = csrr(sepc);
    uint64 scause_val = csrr(scause);
    uint64 stval_val = csrr(stval);
    uint64 sstatus_val = csrr(sstatus);

    /* Sanity checks: */
    if (!(sstatus_val & SSTATUS_SPP))
        panic("kernel_trap: not from S-mode");
    if (sstatus_val & SSTATUS_SIE)
        panic("kernel_trap: interrupts enabled during trap");

    /* Check scause bit 63 to distinguish interrupts vs exceptions. */
    if (scause_val & SCAUSE_INTERRUPT) {
        uint64 code = scause_val & 0xff;
        if (code == IRQ_S_SOFT) {
            csrw(sip, csrr(sip) & (~SIP_SSIP));
            if (++ticks % 100 == 0) {
                kprintf("current ticks=%d\n", ticks);
            }
        } else {
            panic("kernel_trap: unexpected interrupt code=%d", (int)code);
        }
    } else {
        /* Exception — always fatal in kernel mode. */
        panic("kernel_trap: exception scause=%p sepc=%p stval=%p", (void *)scause_val,
              (void *)sepc_val, (void *)stval_val);
    }
}
