#ifndef KPRINTF_H
#define KPRINTF_H

/*
 * kprintf.h — Formatted output for the kernel.
 *
 * Supported format specifiers: %d, %u, %x, %p, %s, %c, %%.
 * Output goes to the UART via uart_putc().
 * See Lecture 1-3 for the full design.
 */

/* Print a formatted string to the console. */
void kprintf(const char *fmt, ...);

/* Print a message and halt the system. */
void panic(const char *fmt, ...);

#endif /* KPRINTF_H */
