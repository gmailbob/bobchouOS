/*
 * trap.c — S-mode trap dispatcher for bobchouOS.
 *
 * Called from kernel_vec (kernel_vec.S) after registers are saved.
 * Reads scause to determine what happened and dispatches accordingly.
 *
 * Interrupt handling: timer ticks via SSIP forwarding (Round 2-3).
 * Exception handling: human-readable diagnostics, breakpoint and
 * ecall-from-S-mode recovery (Round 2-4).
 *
 * Will grow to include usertrap() and devintr() in later phases.
 *
 * Refer to Lectures 2-2, 2-3, and 2-4.
 */

#include "riscv.h"
#include "kprintf.h"

/* ---- Exception name table ----
 *
 * Maps scause exception codes (bit 63 = 0) to human-readable names.
 * Uses C99 designated initializers — entries 10 and 14 (reserved)
 * are left as NULL. See Lecture 2-4, Part 4.
 */
static const char *exc_names[] = {
    [EXC_INST_MISALIGN] = "instruction address misaligned",
    [EXC_INST_ACCESS]   = "instruction access fault",
    [EXC_ILLEGAL_INST]  = "illegal instruction",
    [EXC_BREAKPOINT]    = "breakpoint",
    [EXC_LOAD_MISALIGN] = "load address misaligned",
    [EXC_LOAD_ACCESS]   = "load access fault",
    [EXC_STORE_MISALIGN]= "store/AMO address misaligned",
    [EXC_STORE_ACCESS]  = "store/AMO access fault",
    [EXC_ECALL_U]       = "ecall from U-mode",
    [EXC_ECALL_S]       = "ecall from S-mode",
    [11]                = "ecall from M-mode",
    [EXC_INST_PAGE]     = "instruction page fault",
    [EXC_LOAD_PAGE]     = "load page fault",
    [EXC_STORE_PAGE]    = "store/AMO page fault",
};

#define NUM_EXC (sizeof(exc_names) / sizeof(exc_names[0]))

/*
 * Look up exception name by scause code. Returns "unknown exception"
 * for reserved codes or out-of-range values.
 */
static const char *
exc_name(uint64 scause)
{
    /* TODO: Return the name string from exc_names if scause is in
     * range and the entry is non-NULL. Otherwise return
     * "unknown exception". */
    (void)scause;
    return "unknown exception";
}

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
            /* Timer tick (forwarded from M-mode via SSIP).
             * Clear SSIP so we don't re-trap on sret. */
            csrw(sip, csrr(sip) & ~SIP_SSIP);
            if (++ticks % 100 == 0)
                kprintf("timer: %d seconds\n", (int)(ticks / 100));
        } else {
            panic("kernel_trap: unexpected interrupt code=%d", (int)code);
        }
    } else {
        /* ---- Exception dispatch (Round 2-4) ----
         *
         * Decode scause, detect instruction length at sepc,
         * and dispatch: recoverable exceptions return, fatal ones panic.
         * See Lecture 2-4, Parts 4 and 7.
         */

        /* TODO: Look up the exception name using exc_name(). */

        /* TODO: Detect instruction length at sepc.
         * Read the 16-bit parcel at sepc_val. Check bits 1:0:
         *   0b11 → 4-byte instruction
         *   anything else → 2-byte compressed instruction
         * Store the result in a variable (e.g., inst_len). */

        /* TODO: Dispatch on scause_val using a switch statement.
         *
         * EXC_BREAKPOINT:
         *   - Print a message showing the breakpoint address (sepc_val)
         *   - Advance sepc by inst_len (ebreak may be compressed)
         *   - Return (non-fatal)
         *
         * EXC_ECALL_S:
         *   - Print a message showing the ecall address (sepc_val)
         *   - Advance sepc by 4 (ecall is always 4 bytes)
         *   - Return (non-fatal)
         *
         * default:
         *   - Call panic() with the exception name, scause, sepc, stval
         *     (panic already appends a newline — don't add one in the
         *     format string)
         */

        /* Placeholder: remove this panic once you implement the dispatch. */
        panic("kernel_trap: exception scause=%p sepc=%p stval=%p",
              (void *)scause_val, (void *)sepc_val, (void *)stval_val);
    }
}
