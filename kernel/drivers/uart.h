/*
 * uart.h — UART driver public interface.
 *
 * Provides init, transmit, and receive for the 16550 UART on QEMU virt.
 * See Lecture 1-2 for hardware details.
 */

#ifndef UART_H
#define UART_H

/* Initialize the 16550 UART hardware. Must be called before any I/O. */
void uart_init(void);

/* Transmit one character. Blocks (spins) until the hardware is ready. */
void uart_putc(char c);

/* Receive one character. Returns -1 immediately if nothing available. */
int uart_getc(void);

#endif /* UART_H */
