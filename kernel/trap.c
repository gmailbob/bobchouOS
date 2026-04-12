/*
 * trap.c — S-mode trap dispatcher for bobchouOS.
 *
 * Called from kernelvec (trapvec.S) after registers are saved.
 * Reads scause to determine what happened, prints diagnostics,
 * and halts.  Real handling (timer, exceptions) comes in later rounds.
 *
 * Refer to Lecture 2-2, Part 7.
 */

#include "riscv.h"
#include "kprintf.h"

void
kerneltrap(void)
{
    /* TODO: Read sepc, scause, stval, and sstatus from their CSRs. */

    /* TODO: Sanity checks:
     *   - Verify we came from S-mode (sstatus.SPP should be set).
     *   - Verify interrupts are disabled (sstatus.SIE should be 0). */

    /* TODO: Check scause bit 63 to distinguish interrupts vs exceptions.
     *   - If interrupt: print the interrupt code and halt.
     *   - If exception: print scause, sepc, stval and halt. */
}
