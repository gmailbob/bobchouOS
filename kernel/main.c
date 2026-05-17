/*
 * main.c — Kernel entry point for bobchouOS.
 *
 * kmain() is called by entry.S via mret after switching to S-mode.
 * The stack is already set up and BSS is zeroed.
 */

#include "drivers/uart.h"
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
extern void kernel_vec(void);

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

#ifdef RUN_TESTS
    run_tests();
    kprintf("tests complete, shutting down.\n");
    sbi_shutdown();
#else
    proc_init();
    proc_bootstrap();

    kprintf("starting scheduler...\n");
    scheduler(); /* never returns */
#endif
}
