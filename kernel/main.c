/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S via mret after switching to S-mode.
 * The stack is already set up and BSS is zeroed.
 */

#include "bio.h"
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
#include "vm.h"

#ifdef RUN_TESTS
#include "test/test.h"
#endif

/* Trap vector (defined in kernel_vec.S). */
extern char kernel_vec[];

/* Linker-provided symbols (see linker.ld). */
extern char _kernel_start[];
extern char _kernel_end[];

void
kmain(void) {
    uart_init();

    /* Set stvec to point to kernel_vec (direct mode).
     * stvec is an S-mode concern, so we set it here rather than in
     * entry.S (which is our M-mode boot code). This must happen
     * before anything that could trap. */
    csrw(stvec, (uint64)kernel_vec);

    /* Enable the S-mode timer interrupt (STIE) — SSTC delivers ticks as
     * scause=5. The global gate (SIE) stays off; each thread sets it via
     * intr_on() at its start. */
    csrw(sie, csrr(sie) | SIE_STIE);

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

    /* Device init (after paging — all MMIO-mapped):
     * 1. plic_init        — enable S-mode external interrupts + virtio source
     * 2. virtio_blk_init  — device handshake + queue setup
     * 3. binit            — buffer cache, layered on the driver above (7-2) */
    plic_init();
    virtio_blk_init();
    binit();

#ifdef RUN_TESTS
    run_unit_tests();
#endif

    proc_bootstrap();

#ifdef RUN_TESTS
    proc_create_kernel(run_integration_tests, "integ_test");
#endif

    kprintf("starting scheduler...\n");
    scheduler(); /* never returns */
}
