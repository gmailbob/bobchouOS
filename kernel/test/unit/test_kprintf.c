/*
 * test_kprintf.c -- Smoke tests for kprintf.
 *
 * Since kprintf is used by the test framework itself, we can't assert
 * on its output. Instead we exercise every format specifier and edge
 * case — if nothing crashes and the QEMU console output looks correct,
 * it works. One test_pass is counted for surviving the entire suite.
 */

#include "test/test.h"

void
test_kprintf(void) {
    kprintf("[kprintf] (visual check — verify output below)\n");
    kprintf("  %%d: %d %d %d\n", 0, -1, 2147483647);
    kprintf("  %%u: %u %u\n", 0, 4294967295U);
    kprintf("  %%x: %x %x\n", 0, 0xdeadbeef);
    kprintf("  %%p: %p\n", (void *)0x80000000);
    kprintf("  %%s: \"%s\" \"%s\"\n", "hello", (char *)0);
    kprintf("  %%c: '%c' '%c' '%c'\n", 'A', '0', ' ');
    kprintf("  %%%%: %%\n");
    kprintf("  unknown: %q\n");
    test_pass++; /* survived — count as pass */
}
