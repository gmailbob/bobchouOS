/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S via mret after switching to S-mode.
 * The stack is already set up and BSS is zeroed.
 */

#include "buf.h"
#include "drivers/plic.h"
#include "drivers/uart.h"
#include "drivers/virtio_blk.h"
#include "kalloc.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "mem_layout.h"
#include "proc.h"
#include "riscv.h"
#include "sbi.h"
#include "string.h"
#include "vm.h"

#ifdef RUN_TESTS
#include "test/test.h"
#endif

/* Trap vector (defined in kernel_vec.S). */
extern char kernel_vec[];

/* Linker-provided symbols (see linker.ld). */
extern char _kernel_start[];
extern char _kernel_end[];

/*
 * virtio_smoke_test — kernel thread that reads block 0 and reports.
 *
 * Runs as a real process (PID assigned by proc_create_kernel) because
 * virtio_blk_rw sleeps — that needs a schedulable context with
 * interrupts on, not kmain's bootstrap path.
 *
 * Block 0 will hold the superblock once we have a filesystem (Round
 * 7-4). For now we just confirm the DMA round-trips: status == OK.
 *
 * Temporary scaffolding for Round 7-1 — remove once the buffer cache
 * (7-2) provides a real read path.
 */
static void
virtio_smoke_test(void) {
    intr_on(); /* kernel threads start with interrupts off */

    struct buf *b = kmalloc(sizeof(struct buf));
    if (!b)
        panic("virtio_smoke_test: kmalloc");
    memset(b, 0, sizeof(struct buf));
    b->blockno = 0;
    /* Sentinel: a real transfer overwrites status (0=OK). If the driver
     * is still a stub, this value survives and the test FAILs — so an
     * unimplemented driver reports red, not a false pass. */
    b->status = 0xff;

    virtio_blk_rw(b);

    if (b->status == VIRTIO_BLK_S_OK)
        kprintf("[virtio] read block 0: PASS (first bytes %x %x %x %x)\n", b->data[0], b->data[1],
                b->data[2], b->data[3]);
    else
        kprintf("[virtio] read block 0: FAIL (status=%d) — driver not implemented yet?\n",
                b->status);

    kmfree(b);
    proc_exit(0);
}

void
kmain(void) {
    uart_init();

    /* Set stvec to point to kernel_vec (direct mode).
     * stvec is an S-mode concern, so we set it here rather than in
     * entry.S (which is our M-mode boot code). This must happen
     * before anything that could trap. */
    csrw(stvec, (uint64)kernel_vec);

    /* Enable S-mode software interrupt (SSIP) — this is how M-mode
     * forwards timer events to us. SIE is left disabled here; each
     * kernel thread enables it via intr_on() at its start. */
    csrw(sie, csrr(sie) | SIE_SSIE);

    kprintf("\nbobchouOS is booting...\n");
    kprintf("kernel: %p .. %p (%d bytes)\n", _kernel_start, _kernel_end,
            (int)(_kernel_end - _kernel_start));

    /* Memory subsystem init order matters:
     * 1. kalloc_init         — buddy allocator; everything else calls kalloc()
     * 2. vm_create_kernel_pt — builds page table (needs kalloc for table pages)
     * 3. vm_enable_paging    — activates Sv39 (needs the page table built)
     * 4. kmalloc_init        — slab allocator (needs kalloc for slab pages;
     *    after paging so it works if we later use non-identity mapping) */
    kalloc_init();
    vm_create_kernel_pt();
    vm_enable_paging();
    kmalloc_init();
    proc_init();

    /* Device init (after paging — both are MMIO-mapped):
     * plic_init enables S-mode external interrupts and the virtio
     * source; virtio_blk_init runs the device handshake + queue setup. */
    plic_init();
    virtio_blk_init();

#ifdef RUN_TESTS
    run_tests();
    kprintf("tests complete, shutting down.\n");
    sbi_shutdown();
#else
    proc_bootstrap();
    proc_create_kernel(virtio_smoke_test, "vblk_test"); /* Round 7-1 scaffold */

    kprintf("starting scheduler...\n");
    scheduler(); /* never returns */
#endif
}
