/*
 * plic.c — Platform-Level Interrupt Controller driver for QEMU virt.
 *
 * The PLIC is a memory-mapped device. Its register file (base at
 * PLIC_BASE) has three regions we touch:
 *
 *   Priority:   PLIC_BASE + 4*irq          — priority of source `irq`
 *               (0 = disabled, higher = more urgent)
 *   Enable:     PLIC_BASE + 0x2080 + ctx*0x80 — bitmap of enabled sources
 *               for S-mode context `ctx` (hart 0 S-mode = context 1)
 *   Threshold:  PLIC_BASE + 0x201000 + ctx*0x1000 — context masks sources
 *               at or below this priority
 *   Claim/Comp: PLIC_BASE + 0x201004 + ctx*0x1000 — read to claim,
 *               write to complete
 *
 * QEMU virt context numbering: context 0 = hart 0 M-mode, context 1 =
 * hart 0 S-mode. We run device interrupts in S-mode, so we use context 1.
 *
 * This driver is platform plumbing; the interesting interrupt-handling
 * logic for Round 7-1 lives in virtio_blk.c. See Lecture 7-1, Part 8
 * for both the PLIC register model and the top/bottom-half handling.
 */

#include "drivers/plic.h"
#include "mem_layout.h"
#include "riscv.h"

/* S-mode context for hart 0. Phase 9 will compute this from the hartid. */
#define S_CONTEXT 1

#define PLIC_PRIORITY(irq) (PLIC_BASE + 4 * (irq))
#define PLIC_SENABLE(ctx) (PLIC_BASE + 0x2080 + (ctx)*0x80)
#define PLIC_STHRESHOLD(ctx) (PLIC_BASE + 0x201000 + (ctx)*0x1000)
#define PLIC_SCLAIM(ctx) (PLIC_BASE + 0x201004 + (ctx)*0x1000)

static inline void
plic_write(uint64 addr, uint32 val) {
    *(volatile uint32 *)addr = val;
}

static inline uint32
plic_read(uint64 addr) {
    return *(volatile uint32 *)addr;
}

void
plic_init(void) {
    /* Give the virtio-blk source a nonzero priority (0 means "off"). */
    plic_write(PLIC_PRIORITY(VIRTIO0_IRQ), 1);

    /* Enable the virtio source for hart 0's S-mode context. */
    plic_write(PLIC_SENABLE(S_CONTEXT), 1 << VIRTIO0_IRQ);

    /* Threshold 0: accept any source with priority > 0. */
    plic_write(PLIC_STHRESHOLD(S_CONTEXT), 0);

    /* Enable S-mode external interrupts in sie. The device interrupt
     * will now reach us as scause IRQ_S_EXT (9). */
    csrw(sie, csrr(sie) | SIE_SEIE);
}

int
plic_claim(void) {
    return plic_read(PLIC_SCLAIM(S_CONTEXT));
}

void
plic_complete(int irq) {
    plic_write(PLIC_SCLAIM(S_CONTEXT), irq);
}
