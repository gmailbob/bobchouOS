/*
 * test_kalloc.c -- Tests for the buddy allocator (Rounds 3-2 and 4-2).
 */

#include "test/test.h"
#include "kalloc.h"
#include "riscv.h"
#include "mem_layout.h"
#include "types.h"

void
test_kalloc(void) {
    kprintf("[kalloc]\n");

    /* --- Single-page tests (order 0, same as Phase 3) --- */

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

    /* Order is 0 for single-page allocation. */
    TEST_ASSERT(pg1->order == 0, "order == 0 for single page");

    /* Allocate a second page — should be different from the first. */
    void *p2 = kalloc();
    TEST_ASSERT(p2 != NULL, "second kalloc returns non-NULL");
    TEST_ASSERT(p2 != p1, "second page differs from first");

    /* Free and re-allocate. */
    kfree(p2);
    void *p3 = kalloc();
    TEST_ASSERT(p3 != NULL, "re-alloc after free returns non-NULL");

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

    /* Clean up single-page tests. */
    kfree(p4);
    kfree(p1);

    /* --- Multi-page tests (buddy orders > 0) --- */

    /* Order 1: allocate 2 contiguous pages (8 KB). */
    void *m1 = kalloc_pages(1);
    TEST_ASSERT(m1 != NULL, "kalloc_pages(1) returns non-NULL");
    TEST_ASSERT((uint64)m1 % (PG_SIZE << 1) == 0,
                "order-1 block is 8KB-aligned");
    TEST_ASSERT(pa_to_page((uint64)m1)->order == 1, "order == 1");
    TEST_ASSERT(pa_to_page((uint64)m1)->refcount == 1, "refcount == 1");

    /* The block should be zeroed. */
    bytes = (uint8 *)m1;
    all_zero = 1;
    for (int i = 0; i < PG_SIZE * 2; i++) {
        if (bytes[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    TEST_ASSERT(all_zero, "order-1 block is zeroed");

    kfree_pages(m1, 1);

    /* Order 2: allocate 4 contiguous pages (16 KB). */
    void *m2 = kalloc_pages(2);
    TEST_ASSERT(m2 != NULL, "kalloc_pages(2) returns non-NULL");
    TEST_ASSERT((uint64)m2 % (PG_SIZE << 2) == 0,
                "order-2 block is 16KB-aligned");
    TEST_ASSERT(pa_to_page((uint64)m2)->order == 2, "order == 2");
    kfree_pages(m2, 2);

    /* Buddy merging: allocate two order-0 pages, free both, then
     * allocate order-1. If merging works, this should succeed. */
    void *b1 = kalloc();
    void *b2 = kalloc();
    kfree(b1);
    kfree(b2);
    void *merged = kalloc_pages(1);
    TEST_ASSERT(merged != NULL, "buddy merge allows order-1 after freeing 2 pages");
    kfree_pages(merged, 1);

    /* Splitting: allocate order-0 when only large blocks available.
     * (This is the normal case — large blocks get split down.) */
    void *split = kalloc();
    TEST_ASSERT(split != NULL, "splitting larger block for order-0");
    TEST_ASSERT(pa_to_page((uint64)split)->order == 0, "split result is order 0");
    kfree(split);
}
