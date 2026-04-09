/*
 * main.c — Bare-metal Hello World for RISC-V on QEMU's "virt" machine.
 *
 * How UART output works at the hardware level:
 *
 *   QEMU's "virt" machine includes a 16550 UART (serial port) whose
 *   registers are memory-mapped starting at address 0x10000000.
 *
 *   "Memory-mapped" means the UART looks like a memory address to the CPU.
 *   When we write a byte to 0x10000000, the hardware doesn't store it in
 *   RAM — instead, the UART transmits it as a serial character. QEMU then
 *   displays that character on your terminal.
 *
 *   The 16550 UART has many registers (for baud rate, FIFO control, etc.),
 *   but for basic output we only need one: the Transmit Holding Register
 *   (THR) at offset 0, which is the base address itself.
 *
 *   We declare the pointer as "volatile" to tell the compiler: "this address
 *   has side effects — don't optimize away writes to it, and don't reorder
 *   them." Without volatile, the compiler might decide that writing to the
 *   same address repeatedly is redundant and eliminate our output.
 */

#define UART0_ADDR 0x10000000

/*
 * uart_putc — Send a single character to the serial console.
 *
 * This is the most primitive output function possible: a raw write to a
 * hardware register. Every other printing function (uart_puts, kprintf
 * in the real kernel) will ultimately call something like this.
 */
void
uart_putc(char c) {
    volatile char *uart = (volatile char *)UART0_ADDR;
    *uart = c;
}

/*
 * uart_puts — Send a null-terminated string to the serial console.
 */
void
uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s);
        s++;
    }
}

/*
 * main — Kernel entry point (called from entry.S after stack setup).
 *
 * In a real OS, main() would initialize subsystems (memory, interrupts,
 * processes) and then start the scheduler. Here, we just say hello and halt.
 */
void
main(void) {
    uart_puts("Hello from bobchouOS!\n");
}
