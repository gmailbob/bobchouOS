/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S after the stack is set up and BSS is zeroed.
 * Round 2: uses the proper UART driver instead of raw writes.
 */

#include "drivers/uart.h"

/* Print a null-terminated string using the UART driver. */
static void
uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

void
kmain(void) {
    uart_init();
    uart_puts("hello from bobchouOS\n");

    /* Nothing else to do — halt. */
    for (;;)
        ;
}
