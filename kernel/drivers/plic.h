/*
 * plic.h — Platform-Level Interrupt Controller (PLIC) interface.
 *
 * The PLIC routes external device interrupts (UART, virtio, etc.) to
 * a hart's S-mode external-interrupt line. Our first device-interrupt
 * source is virtio-blk (Round 7-1), so the PLIC arrives here even
 * though the controller itself is platform infrastructure.
 *
 * See Lecture 7-1, Part 8 "How the interrupt reaches us: PLIC" for the
 * model: source priority → enable bit → per-hart threshold → claim/complete.
 */

#ifndef PLIC_H
#define PLIC_H

/* Interrupt source numbers on QEMU's `virt` machine. */
#define UART0_IRQ 10
#define VIRTIO0_IRQ 1

/* Initialize the PLIC: set source priorities, enable the sources we
 * care about for hart 0's S-mode context, and drop the threshold to 0.
 * Call once at boot, after paging is enabled (PLIC is MMIO-mapped). */
void plic_init(void);

/* Claim the highest-priority pending interrupt for this hart's S-mode
 * context. Returns the source number, or 0 if none pending. */
int plic_claim(void);

/* Signal completion of handling for source `irq` (re-enables it). */
void plic_complete(int irq);

#endif /* PLIC_H */
