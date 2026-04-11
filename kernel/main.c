/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S after the stack is set up and BSS is zeroed.
 * Round 3: uses kprintf for formatted output.
 */

#include "drivers/uart.h"
#include "kprintf.h"

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
    kprintf("\n");

    /* Kernel memory layout — a real use of %p and %d. */
    kprintf("kernel: %p .. %p (%d bytes)\n",
            _kernel_start, _kernel_end,
            (int)(_kernel_end - _kernel_start));
    kprintf("  bss:  %p .. %p (%d bytes)\n",
            _bss_start, _bss_end,
            (int)(_bss_end - _bss_start));
    kprintf("UART:   %p\n", (void *)0x10000000);
    kprintf("\n");

    /* Exercise every format specifier. */
    kprintf("--- kprintf test ---\n");
    kprintf("char:     '%c'\n", 'A');
    kprintf("string:   '%s'\n", "hello");
    kprintf("null str: '%s'\n", (char *)0);
    kprintf("decimal:  %d %d %d\n", 0, 42, -1);
    kprintf("unsigned: %u\n", 4294967295u);
    kprintf("hex:      0x%x 0x%x\n", 255, 0xDEADBEEF);
    kprintf("pointer:  %p\n", (void *)0x80001000);
    kprintf("percent:  100%%\n");
    kprintf("unknown:  %q\n");
    kprintf("--- end test ---\n");

    /* Nothing else to do — halt. */
    for (;;)
        ;
}
