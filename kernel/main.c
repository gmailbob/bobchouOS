/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S via mret after switching to S-mode.
 * The stack is already set up and BSS is zeroed.
 */

#include "drivers/uart.h"
#include "kprintf.h"
#include "riscv.h"

#ifdef RUN_TESTS
#include "test/test.h"
#endif

/* Trap vector (defined in kernel_vec.S). */
extern void kernel_vec(void);

/* Linker-provided symbols (see linker.ld). */
extern char _kernel_start[];
extern char _kernel_end[];

void
kmain(void) {
    uart_init();

    /* Set stvec to point to kernel_vec (direct mode).
     * stvec is an S-mode concern, so we set it here rather than in
     * entry.S (which is our M-mode boot code). This must happen
     * before anything that could trap. */
    csrw(stvec, (uint64)kernel_vec);

    /* Enable S-mode interrupts for timer ticks. */
    csrw(sie, csrr(sie) | SIE_SSIE);
    csrw(sstatus, csrr(sstatus) | SSTATUS_SIE);

    kprintf("\nbobchouOS is booting...\n");
    kprintf("running in S-mode\n");
    /* Read sstatus to confirm we're in S-mode (or higher). */
    kprintf("sstatus=%p\n", (void *)csrr(sstatus));
    kprintf("kernel: %p .. %p (%d bytes)\n", _kernel_start, _kernel_end,
            (int)(_kernel_end - _kernel_start));

#ifdef RUN_TESTS
    run_tests();
#endif

    kprintf("\ntimer interrupts enabled, waiting for ticks...\n");

    /* Spin forever — timer interrupts will fire periodically. */
    for (;;)
        asm volatile("wfi");
}
