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
    /* TODO: Write character c to the UART0 address.
     *
     * Hint: cast UART0 to a volatile uint8 pointer and dereference it.
     * This is the same pattern from Phase 0's bare-metal hello.
     */
}

/* Print a null-terminated string using the raw UART write. */
static void
uart_puts_raw(const char *s) {
    /* TODO: Loop over each character in s and call uart_putc_raw().
     * Stop when you hit the null terminator '\0'.
     */
}

void
kmain(void) {
    /* TODO: Print a message to prove the boot path works.
     *
     * Call uart_puts_raw() with a string like "hello from bobchouOS\n".
     * If you see this on the QEMU console, entry.S and the linker script
     * are working correctly.
     */

    /* Nothing else to do — halt. */
    for (;;)
        ;
}
