/*
 * trap.c — S-mode trap dispatcher for bobchouOS.
 *
 * Kernel trap path:
 *   kernel_vec → kernel_trap (dispatch) → kernel_trap_ret (scheduling)
 *
 * User trap path:
 *   user_vec → user_trap (dispatch) → user_trap_ret (scheduling + sret)
 *
 * See Lectures 2-2, 2-3, 2-4, and 6-1.
 */

#include "riscv.h"
#include "kprintf.h"
#include "mem_layout.h"
#include "proc.h"
#include "trapframe.h"

/* ---- Exception name table ----
 *
 * Maps scause exception codes (bit 63 = 0) to human-readable names.
 * Uses C99 designated initializers — entries 10 and 14 (reserved)
 * are left as NULL. See Lecture 2-4, Part 4.
 */
// clang-format off
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
// clang-format on

#define NUM_EXC (sizeof(exc_names) / sizeof(exc_names[0]))

/*
 * Look up exception name by scause code. Returns "unknown exception"
 * for reserved codes or out-of-range values.
 */
static const char *
exc_name(uint64 scause) {
    if (scause < NUM_EXC && exc_names[scause])
        return exc_names[scause];
    return "unknown exception";
}

void
kernel_trap(void) {
    uint64 sepc_val = csrr(sepc);
    uint64 scause_val = csrr(scause);
    uint64 stval_val = csrr(stval);
    uint64 sstatus_val = csrr(sstatus);
    uint64 code = scause_val & 0xff;

    /* Sanity checks: */
    if (!(sstatus_val & SSTATUS_SPP))
        panic("kernel_trap: not from S-mode (sepc=%p scause=%p)", (void *)sepc_val,
              (void *)scause_val);
    if (sstatus_val & SSTATUS_SIE)
        panic("kernel_trap: interrupts enabled during trap");

    /* Check scause bit 63 to distinguish interrupts vs exceptions. */
    if (scause_val & SCAUSE_INTERRUPT) {
        switch (code) {
        case IRQ_S_SOFT:
            /* Timer tick (forwarded from M-mode via SSIP).
             * Clear SSIP so we don't re-trap on sret. */
            csrw(sip, csrr(sip) & ~SIP_SSIP);
            if (this_cpu()->proc)
                this_cpu()->need_resched = 1;
            break;

            /* IRQ_S_EXT: PLIC external interrupts — future round */

        default:
            panic("kernel_trap: unexpected interrupt code=%d", (int)code);
        }
    } else {
        /* ---- Exception dispatch (Round 2-4) ----
         *
         * Decode scause, detect instruction length at sepc,
         * and dispatch: recoverable exceptions return, fatal ones panic.
         * See Lecture 2-4, Parts 4 and 7.
         */
        const char *name = exc_name(scause_val);

        /* Detect instruction length at sepc (4-byte vs 2-byte compressed).
         * Read the first 16-bit parcel; bits 1:0 = 0x3 means 4-byte. */
        uint16 inst = *(uint16 *)sepc_val;
        int inst_len = (inst & 0x3) == 0x3 ? 4 : 2;

        switch (code) {
        case EXC_BREAKPOINT:
            kprintf("kernel_trap: %s at %p\n", name, (void *)sepc_val);
            csrw(sepc, sepc_val + inst_len);
            break;

            /* EXC_ECALL_S never reaches here — hardware routes S-mode ecall
             * directly to M-mode (see entry.S Step 7: medeleg). */

        default:
            panic("kernel_trap: %s  scause=%p sepc=%p stval=%p", name, (void *)scause_val,
                  (void *)sepc_val, (void *)stval_val);
        }
    }
}

/*
 * kernel_trap_ret — called from kernel_vec after kernel_trap returns.
 *
 * Two checks before returning to the interrupted code:
 * 1. If p->killed is set, the process exits immediately (deferred kill).
 * 2. If need_resched is set, yield the CPU (timer preemption).
 *
 * yield() may swtch to a user process whose user_trap_ret modifies
 * sepc/sstatus. We save/restore these CSRs around yield so kernel_vec's
 * sret uses the correct values.
 */
void
kernel_trap_ret(void) {
    struct cpu *c = this_cpu();
    if (c->proc && c->proc->killed)
        proc_exit(-1);
    if (c->need_resched && c->proc) {
        c->need_resched = 0;
        /* Save sepc/sstatus: yield may swtch to a user process whose
         * user_trap_ret modifies these CSRs. When we resume, kernel_vec's
         * sret needs the original values. */
        uint64 sepc_saved = csrr(sepc);
        uint64 sstatus_saved = csrr(sstatus);
        yield();
        csrw(sepc, sepc_saved);
        csrw(sstatus, sstatus_saved);
    }
}

/* --- User-mode trap handling (Round 6-1) --- */

/* Kernel trap vector (defined in kernel_vec.S). */
extern char kernel_vec[];

/* Trampoline symbols (defined in trampoline.S). */
extern char user_vec[];
extern char user_ret[];

/* Forward declaration (user_trap calls user_trap_ret). */
void user_trap_ret(void);

/* Syscall dispatch (defined in syscall.c). */
int64 syscall(void);

/*
 * user_trap — Dispatch traps from user mode.
 *
 * Called from user_vec after registers are saved to trapframe and
 * the page table is switched to kernel. This function is pure dispatch:
 * it identifies the cause and handles it, then falls through to
 * user_trap_ret for scheduling decisions and return to user mode.
 *
 * See Lecture 6-1, Part 7.
 */
void
user_trap(void) {
    /* Arm kernel trap handler for any nested traps (e.g., timer during syscall). */
    csrw(stvec, (uint64)kernel_vec);

    uint64 scause_val = csrr(scause);
    uint64 sstatus_val = csrr(sstatus);
    uint64 code = scause_val & 0xff;

    struct proc *p = this_proc();
    /* Save user PC before yield could overwrite sepc. */
    p->trapframe->epc = csrr(sepc);

    /* Sanity: user_vec only fires from U-mode with interrupts off. */
    if (sstatus_val & SSTATUS_SPP)
        panic("user_trap: not from U-mode");
    if (sstatus_val & SSTATUS_SIE)
        panic("user_trap: interrupts enabled during trap");

    if (scause_val & SCAUSE_INTERRUPT) {
        switch (code) {
        case IRQ_S_SOFT:
            csrw(sip, csrr(sip) & ~SIP_SSIP);
            this_cpu()->need_resched = 1;
            break;
        default:
            kprintf("user_trap: unexpected interrupt code=%d\n", (int)code);
            p->killed = 1;
            break;
        }
    } else {
        switch (code) {
        case EXC_ECALL_U:
            /* ecall is 4 bytes; advance past it so sret resumes at next insn. */
            p->trapframe->epc += 4;
            intr_on();
            p->trapframe->a0 = syscall();
            break;
        default:
            kprintf("user_trap: exception pid=%d scause=%p sepc=%p stval=%p\n", p->pid,
                    (void *)scause_val, (void *)p->trapframe->epc, (void *)csrr(stval));
            p->killed = 1;
            break;
        }
    }

    user_trap_ret();
}

/*
 * user_trap_ret — Scheduling decisions + return to user mode.
 *
 * The single exit point for ALL paths to user mode: syscalls, interrupts,
 * exceptions, and the first-ever entry to user mode.
 *
 * Two phases:
 *   Phase 1 (scheduling): check killed → exit; check need_resched → yield
 *   Phase 2 (return setup): disable interrupts, set stvec to trampoline,
 *     fill trapframe kernel fields, set sstatus/sepc, call user_ret
 *
 * Never returns — user_ret ends with sret.
 *
 * See Lecture 6-1, Part 7.
 */
void
user_trap_ret(void) {
    struct cpu *c = this_cpu();
    struct proc *p = c->proc;

    /* Phase 1: scheduling decisions.
     * stvec still = kernel_vec here, so yield/exit can safely take
     * kernel traps. Must happen BEFORE we set stvec to trampoline. */
    if (p->killed)
        proc_exit(-1);
    if (c->need_resched) {
        c->need_resched = 0;
        yield();
    }

    /* Phase 2: point of no return — set up hardware state for user mode.
     * After this, no yields or exits (stvec = trampoline, SPP = 0). */
    intr_off();
    csrw(stvec, TRAMPOLINE);  /* next user trap → user_vec */
    p->trapframe->hartid = 0; /* Phase 9: read_tp() for multi-hart */
    csrw(sstatus, (csrr(sstatus) & ~SSTATUS_SPP) | SSTATUS_SPIE);
    csrw(sepc, p->trapframe->epc); /* sret will jump here */

    /* Call user_ret at the TRAMPOLINE VA (not identity-mapped address)
     * so execution survives the satp switch inside user_ret. */
    uint64 user_ret_va = TRAMPOLINE + ((uint64)user_ret - (uint64)user_vec);
    ((void (*)(uint64, uint64))user_ret_va)(TRAPFRAME, MAKE_SATP(p->pagetable));
}
