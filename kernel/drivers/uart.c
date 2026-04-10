/*
 * uart.c — 16550 UART driver for QEMU virt machine.
 *
 * Phase 1: polling only, no interrupts.
 * See Lecture 1-2 for the register map and initialization sequence.
 */

#include "types.h"

/* ---- Hardware constants ---- */

#define UART0 0x10000000UL

/* Register offsets */
#define RHR 0 /* Receive Holding Register  (read,  DLAB=0) */
#define THR 0 /* Transmit Holding Register (write, DLAB=0) */
#define DLL 0 /* Divisor Latch Low         (r/w,   DLAB=1) */
#define IER 1 /* Interrupt Enable Register (r/w,   DLAB=0) */
#define DLM 1 /* Divisor Latch High        (r/w,   DLAB=1) */
#define FCR 2 /* FIFO Control Register     (write        ) */
#define LCR 3 /* Line Control Register     (r/w          ) */
#define LSR 5 /* Line Status Register      (read         ) */

/* LSR flags */
#define LSR_RX_READY (1 << 0) /* Data available in RHR */
#define LSR_TX_IDLE (1 << 5)  /* THR is empty */

/* ---- Register access helpers ---- */

static inline uint8
uart_read_reg(uint32 reg) {
    return *(volatile uint8 *)(UART0 + reg);
}

static inline void
uart_write_reg(uint32 reg, uint8 val) {
    *(volatile uint8 *)(UART0 + reg) = val;
}

/* ---- Public API ---- */

/*
 * Initialize the 16550 UART.
 *
 * Sequence: disable interrupts, set baud rate to 38400 (divisor 3),
 * configure 8N1 data format, enable and reset FIFOs.
 * Refer to Lecture 1-2 Part 4 for the step-by-step explanation.
 */
void
uart_init(void) {
    uart_write_reg(IER, 0);

    uart_write_reg(LCR, 0x80);
    uart_write_reg(DLL, 0x03);
    uart_write_reg(DLM, 0);

    uart_write_reg(LCR, 0x03);
    uart_write_reg(FCR, 0x07);
}

/*
 * Transmit one character.
 * Spin until THR is empty (check LSR), then write to THR.
 */
void
uart_putc(char c) {
    while (!(uart_read_reg(LSR) & LSR_TX_IDLE))
        ;
    uart_write_reg(THR, c);
}

/*
 * Receive one character (non-blocking).
 * If data is available (check LSR), read from RHR and return it.
 * Otherwise return -1.
 */
int
uart_getc(void) {
    if (!(uart_read_reg(LSR) & LSR_RX_READY))
        return -1;
    return uart_read_reg(RHR);
}
