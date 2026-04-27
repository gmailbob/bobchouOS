/*
 * test_kalloc.c -- Tests for the physical page allocator (Round 3-2).
 */

#include "test/test.h"
#include "kalloc.h"
#include "riscv.h"
#include "mem_layout.h"
#include "types.h"

void
test_kalloc(void) {
    kprintf("[kalloc]\n");

    /* Basic allocation: kalloc returns non-NULL, page-aligned address. */
    void *p1 = kalloc();
    TEST_ASSERT(p1 != NULL, "kalloc returns non-NULL");
    TEST_ASSERT((uint64)p1 % PG_SIZE == 0, "kalloc returns page-aligned");

    /* Address is in the free region (above kernel, below PHYS_STOP). */
    TEST_ASSERT((uint64)p1 >= KERN_BASE, "page above KERN_BASE");
    TEST_ASSERT((uint64)p1 < PHYS_STOP, "page below PHYS_STOP");

    /* Returned page is zeroed. */
    uint8 *bytes = (uint8 *)p1;
    int all_zero = 1;
    for (int i = 0; i < PG_SIZE; i++) {
        if (bytes[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    TEST_ASSERT(all_zero, "kalloc returns zeroed page");

    /* Refcount is 1 after allocation. */
    struct page *pg1 = pa_to_page((uint64)p1);
    TEST_ASSERT(pg1->refcount == 1, "refcount == 1 after kalloc");

    /* Allocate a second page — should be different from the first. */
    void *p2 = kalloc();
    TEST_ASSERT(p2 != NULL, "second kalloc returns non-NULL");
    TEST_ASSERT(p2 != p1, "second page differs from first");

    /* Free and re-allocate: LIFO means we get p2 back. */
    kfree(p2);
    void *p3 = kalloc();
    TEST_ASSERT(p3 == p2, "LIFO: re-alloc returns last freed page");

    /* Freed page is re-zeroed (not still filled with 0x01 junk). */
    bytes = (uint8 *)p3;
    all_zero = 1;
    for (int i = 0; i < PG_SIZE; i++) {
        if (bytes[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    TEST_ASSERT(all_zero, "re-allocated page is zeroed");

    /* Refcount is 0 after free, 1 after re-alloc. */
    kfree(p3);
    TEST_ASSERT(pa_to_page((uint64)p3)->refcount == 0, "refcount == 0 after kfree");
    void *p4 = kalloc();
    TEST_ASSERT(pa_to_page((uint64)p4)->refcount == 1, "refcount == 1 after re-alloc");

    /* Clean up: free everything we allocated. */
    kfree(p4);
    kfree(p1);
}
