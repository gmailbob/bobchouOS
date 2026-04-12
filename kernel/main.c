/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S via mret after switching to S-mode.
 * The stack is already set up and BSS is zeroed.
 */

#include "drivers/uart.h"
#include "kprintf.h"
#include "riscv.h"

/* Linker-provided symbols (see linker.ld). */
extern char _kernel_start[];
extern char _kernel_end[];
extern char _bss_start[];
extern char _bss_end[];

void
kmain(void) {
    uart_init();

    kprintf("\n");
    kprintf("bobchouOS is booting...\n");
    kprintf("running in S-mode\n");
    kprintf("\n");

    /* Read sstatus to confirm we're in S-mode (or higher).
     * If we were in U-mode, this CSR read would fault. */
    uint64 sstatus_val = csrr(sstatus);
    kprintf("sstatus = %p\n", (void *)sstatus_val);

    /* Kernel memory layout. */
    kprintf("kernel: %p .. %p (%d bytes)\n", _kernel_start, _kernel_end,
            (int)(_kernel_end - _kernel_start));
    kprintf("  bss:  %p .. %p (%d bytes)\n", _bss_start, _bss_end, (int)(_bss_end - _bss_start));
    kprintf("UART:   %p\n", (void *)0x10000000);
    kprintf("\n");

    /* Nothing else to do — halt. */
    for (;;)
        ;
}
