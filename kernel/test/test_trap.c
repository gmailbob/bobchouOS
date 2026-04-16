/*
 * test_trap.c -- Tests for exception handling (Round 2-4).
 *
 * Tests recoverable exceptions (ebreak, ecall from S-mode) by
 * triggering them and asserting we survive. The illegal instruction
 * test is commented out because it panics (fatal), killing the
 * test runner — uncomment temporarily to verify panic output.
 */

#include "test/test.h"

void
test_trap(void) {
    kprintf("[trap]\n");

    /* Breakpoint: handler should print address, advance sepc, return. */
    asm volatile("ebreak");
    TEST_ASSERT(1, "ebreak survived");

    /* Ecall from S-mode: handler should print, advance sepc, return.
     * This works because our medeleg has bit 9 set (0xffff). */
    asm volatile("ecall");
    TEST_ASSERT(1, "ecall from S-mode survived");

    /* Illegal instruction: fatal — visual check only.
     * Uncomment to verify the panic message is human-readable,
     * then comment back out so the test runner can continue.
     *
     * asm volatile("csrr t0, mhartid");
     */
}
