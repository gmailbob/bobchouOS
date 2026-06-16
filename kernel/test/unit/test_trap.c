/*
 * test_trap.c -- Tests for exception and timer-CSR handling.
 *
 * Tests recoverable exceptions (ebreak) and the SSTC timer path (stimecmp)
 * by triggering them and asserting we survive / round-trip correctly.
 */

#include "test/test.h"
#include "riscv.h"

void
test_trap(void) {
    kprintf("[trap]\n");

    /* Breakpoint: S-mode handler advances sepc and returns. */
    asm volatile("ebreak");
    TEST_ASSERT(1, "ebreak survived");

    /* SSTC timer: S-mode writes stimecmp directly and reads it back.
     * Deadline parked far ahead so it won't fire during the test. */
    uint64 deadline = read_time() + 1000000000UL;
    set_timer(deadline);
    TEST_ASSERT(csrr(stimecmp) == deadline, "stimecmp round-trips the deadline");
}
