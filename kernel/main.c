/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S after the stack is set up and BSS is zeroed.
 * For Round 1, we just do a raw UART write to prove the boot path works.
 * The proper UART driver and kprintf come in Rounds 2 and 3.
 */

#include "types.h"

/* UART0 address on QEMU's virt machine. */
#define UART0 0x10000000UL

/*
 * Write a single character to the UART.
 * This is a temporary raw write — no init, no TX-ready check.
 * It works on QEMU because the virtual UART is always ready.
 * Replaced by the proper UART driver in Round 2.
 */
static void
uart_putc_raw(char c) {
    volatile uint8 *port = (volatile uint8 *)UART0;
    *port = c;
}

/* Print a null-terminated string using the raw UART write. */
static void
uart_puts_raw(const char *s) {
    const char *c = s;
    while (*c)
        uart_putc_raw(*c++);
}

void
kmain(void) {
    uart_puts_raw("hello from bobchouOS\n");

    /* Nothing else to do — halt. */
    for (;;)
        ;
}
