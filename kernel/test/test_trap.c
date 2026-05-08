/*
 * test_trap.c -- Tests for exception and SBI handling.
 *
 * Tests recoverable exceptions (ebreak) and mini-SBI ecall (set_timer)
 * by triggering them and asserting we survive.
 */

#include "test/test.h"
#include "riscv.h"
#include "sbi.h"

void
test_trap(void) {
    kprintf("[trap]\n");

    /* Breakpoint: S-mode handler advances sepc and returns. */
    asm volatile("ebreak");
    TEST_ASSERT(1, "ebreak survived");

    /* SBI ecall: traps to M-mode, M-mode writes mtimecmp and returns.
     * Verify by reading mtimecmp back — it should match our deadline. */
    uint64 now = read_mtime();
    uint64 deadline = now + 1000000000UL;
    sbi_set_timer(deadline);
    uint64 mtimecmp_val = *(volatile uint64 *)CLINT_MTIMECMP(0);
    TEST_ASSERT(mtimecmp_val == deadline, "sbi_set_timer wrote correct deadline");
}
