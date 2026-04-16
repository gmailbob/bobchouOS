/*
 * kprintf.c — Formatted output for the kernel.
 *
 * Phase 1: no locking (single hart). We will add a spinlock in Phase 2.
 * See Lecture 1-3 for the format string parsing and integer conversion.
 */

#include <stdarg.h>
#include "drivers/uart.h"

/* ---- Digit lookup table ---- */

static const char digits[] = "0123456789abcdef";

/* ---- Internal helpers ---- */

/*
 * Print an unsigned integer in the given base (10 or 16).
 * Uses the buffer-and-reverse approach: extract digits with repeated
 * division, collect them in buf[], then print in reverse order.
 */
static void
print_uint(unsigned long val, int base) {
    char buf[20];
    int i = 0;
    do {
        buf[i++] = digits[val % base];
        val /= base;
    } while (val);
    while (--i >= 0)
        uart_putc(buf[i]);
}

/*
 * Print a signed integer in the given base.
 */
static void
print_int(long val, int base) {
    if (base != 10 || val >= 0)
        return print_uint((unsigned long)val, base);
    uart_putc('-');
    print_uint((unsigned long)(-val), base);
}

/*
 * Core formatting engine. Walks the format string, dispatches on
 * specifiers, and calls uart_putc / print_uint / print_int.
 */
static void
vprintfmt(const char *fmt, va_list ap) {
    /*
     * Using char here is fine: the char->int->char round trip through
     * uart_putc preserves the original byte on both signed and unsigned
     * char systems. The C standard library uses int (e.g., getchar/putchar)
     * to represent EOF (-1) alongside all 256 byte values — we don't need
     * that here since we stop at the null terminator.
     */
    char c;
    while ((c = *fmt++)) {
        if (c != '%') {
            uart_putc(c);
            continue;
        }

        switch ((c = *fmt++)) {
        case 'd':
            print_int(va_arg(ap, int), 10);
            break;
        case 'u':
            print_uint(va_arg(ap, unsigned int), 10);
            break;
        case 'x':
            print_uint(va_arg(ap, unsigned int), 16);
            break;
        case 'p':
            uart_putc('0');
            uart_putc('x');
            print_uint(va_arg(ap, unsigned long), 16);
            break;
        case 's': {
            char *s = va_arg(ap, char *);
            if (!s)
                s = "(null)";
            while (*s)
                uart_putc(*s++);
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case '%':
            uart_putc('%');
            break;
        case '\0':
            return;
        default:
            uart_putc('%');
            uart_putc(c);
            break;
        }
    }
}

/* ---- Public API ---- */

/*
 * Formatted output to the console.
 */
void
kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintfmt(fmt, ap);
    va_end(ap);
}

/*
 * Print a panic message and halt.
 */
void
panic(const char *fmt, ...) {
    kprintf("PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    vprintfmt(fmt, ap);
    va_end(ap);
    kprintf("\n");

    for (;;)
        asm volatile("wfi");
}
