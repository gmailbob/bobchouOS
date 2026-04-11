/*
 * kprintf.c — Formatted output for the kernel.
 *
 * Phase 1: no locking (single hart). We will add a spinlock in Phase 2.
 * See Lecture 1-3 for the format string parsing and integer conversion.
 */

#include <stdarg.h>
#include "types.h"
#include "drivers/uart.h"

/* ---- Digit lookup table ---- */

static const char digits[] = "0123456789abcdef";

/* ---- Internal helpers ---- */

/*
 * Print an unsigned integer in the given base (10 or 16).
 * Uses the buffer-and-reverse approach: extract digits with repeated
 * division, collect them in buf[], then print in reverse order.
 *
 * TODO: Implement this function.
 * Hints:
 *   - Use a do-while loop so that val == 0 prints "0".
 *   - buf[i++] = digits[val % base]; val /= base;
 *   - Then print buf[] backwards with uart_putc().
 *   - Buffer size 20 is enough for any 64-bit value.
 */
static void
print_uint(unsigned long val, int base)
{
    /* TODO */
}

/*
 * Print a signed integer in the given base.
 *
 * TODO: Implement this function.
 * Hints:
 *   - If base is not 10, treat val as unsigned (cast and call print_uint).
 *   - If val is negative, print '-' and negate (cast to unsigned long).
 *   - Otherwise, call print_uint directly.
 */
static void
print_int(long val, int base)
{
    /* TODO */
}

/*
 * Core formatting engine. Walks the format string, dispatches on
 * specifiers, and calls uart_putc / print_uint / print_int.
 *
 * TODO: Implement this function.
 * Hints:
 *   - Loop through fmt one character at a time.
 *   - If the character is not '%', output it with uart_putc().
 *   - If it is '%', advance to the next character and switch:
 *       'd' -> print_int(va_arg(ap, int), 10)
 *       'u' -> print_uint(va_arg(ap, unsigned int), 10)
 *       'x' -> print_uint(va_arg(ap, unsigned int), 16)
 *       'p' -> print "0x", then print_uint(va_arg(ap, unsigned long), 16)
 *       's' -> loop through the string with uart_putc; print "(null)" if NULL
 *       'c' -> uart_putc(va_arg(ap, int))  (int because of promotion)
 *       '%' -> uart_putc('%')
 *       default -> uart_putc('%'), uart_putc(c)  (print unknown specifier literally)
 *   - If '%' is the last character in fmt (nothing after it), break.
 */
static void
vprintfmt(const char *fmt, va_list ap)
{
    /* TODO */
}

/* ---- Public API ---- */

/*
 * Formatted output to the console.
 *
 * TODO: Implement this function.
 * Hints:
 *   - Declare a va_list, call va_start, call vprintfmt, call va_end.
 */
void
kprintf(const char *fmt, ...)
{
    /* TODO */
}

/*
 * Print a panic message and halt.
 *
 * TODO: Implement this function.
 * Hints:
 *   - Print "PANIC: " with kprintf.
 *   - Use va_start/vprintfmt/va_end to print the formatted message.
 *   - Print a newline with kprintf.
 *   - Halt with for (;;) ;
 */
void
panic(const char *fmt, ...)
{
    /* TODO */
}
