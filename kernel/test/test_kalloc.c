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
    TEST_ASSERT((uint64)m1 % (PG_SIZE << 1) == 0, "order-1 block is 8KB-aligned");
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
    TEST_ASSERT((uint64)m2 % (PG_SIZE << 2) == 0, "order-2 block is 16KB-aligned");
    TEST_ASSERT(pa_to_page((uint64)m2)->order == 2, "order == 2");
    kfree_pages(m2, 2);

    /* --- MAX_ORDER allocation (order 10 = 1024 pages = 4 MB) --- */

    void *big = kalloc_pages(MAX_ORDER);
    TEST_ASSERT(big != NULL, "kalloc_pages(MAX_ORDER) returns non-NULL");
    TEST_ASSERT((uint64)big % (PG_SIZE << MAX_ORDER) == 0, "MAX_ORDER block is properly aligned");
    TEST_ASSERT(pa_to_page((uint64)big)->order == MAX_ORDER, "order == MAX_ORDER");
    TEST_ASSERT(pa_to_page((uint64)big)->refcount == 1, "MAX_ORDER refcount == 1");
    kfree_pages(big, MAX_ORDER);

    /* --- Buddy merging --- */

    /* Allocate two order-0 pages, free both, then allocate order-1.
     * If merging works, this should succeed. */
    void *b1 = kalloc();
    void *b2 = kalloc();
    kfree(b1);
    kfree(b2);
    void *merged = kalloc_pages(1);
    TEST_ASSERT(merged != NULL, "buddy merge allows order-1 after freeing 2 pages");
    kfree_pages(merged, 1);

    /* --- Splitting --- */

    /* Allocate order-0 when only large blocks available.
     * (This is the normal case — large blocks get split down.) */
    void *split = kalloc();
    TEST_ASSERT(split != NULL, "splitting larger block for order-0");
    TEST_ASSERT(pa_to_page((uint64)split)->order == 0, "split result is order 0");
    kfree(split);

    /* --- Free and re-alloc multi-page block --- */

    void *mp1 = kalloc_pages(3);
    TEST_ASSERT(mp1 != NULL, "alloc order-3 (8 pages)");
    kfree_pages(mp1, 3);
    void *mp2 = kalloc_pages(3);
    TEST_ASSERT(mp2 != NULL, "re-alloc order-3 after free");
    TEST_ASSERT(mp2 == mp1, "re-alloc returns same block");
    kfree_pages(mp2, 3);

    /* --- Cascade merge: alloc 4 order-0 pages, free all, alloc order-2 --- */

    void *c[4];
    for (int i = 0; i < 4; i++)
        c[i] = kalloc();
    for (int i = 0; i < 4; i++)
        kfree(c[i]);
    void *c_merged = kalloc_pages(2);
    TEST_ASSERT(c_merged != NULL, "cascade merge: 4 order-0 → order-2");
    kfree_pages(c_merged, 2);

    /* --- Deep split: alloc order-0 from a high-order block --- */

    /* Grab a large block to force later order-0 to split from high up. */
    void *hog1 = kalloc_pages(8);
    void *hog2 = kalloc_pages(8);
    TEST_ASSERT(hog1 != NULL && hog2 != NULL, "alloc two order-8 blocks");
    void *deep = kalloc();
    TEST_ASSERT(deep != NULL, "order-0 alloc after large blocks taken");
    TEST_ASSERT(pa_to_page((uint64)deep)->order == 0, "deep split gives order 0");
    kfree(deep);
    kfree_pages(hog2, 8);
    kfree_pages(hog1, 8);

    /* --- OOM: exhaust memory then check NULL return --- */

    /* Drain all memory order by order, from MAX_ORDER down to 0.
     * We store pointers per order (max ~32 blocks at order 10,
     * a few at smaller orders). Total array size stays small. */
    struct {
        void *ptrs[64];
        int count;
    } saved[MAX_ORDER + 1];
    for (int o = 0; o <= MAX_ORDER; o++)
        saved[o].count = 0;

    for (int o = MAX_ORDER; o >= 0; o--) {
        while (saved[o].count < 64) {
            void *p = kalloc_pages(o);
            if (!p)
                break;
            saved[o].ptrs[saved[o].count++] = p;
        }
    }

    /* Now everything should be exhausted. */
    void *oom = kalloc();
    TEST_ASSERT(oom == NULL, "OOM: kalloc returns NULL when exhausted");

    void *oom_big = kalloc_pages(1);
    TEST_ASSERT(oom_big == NULL, "OOM: kalloc_pages returns NULL when exhausted");

    /* Free everything back. */
    for (int o = 0; o <= MAX_ORDER; o++)
        for (int i = 0; i < saved[o].count; i++)
            kfree_pages(saved[o].ptrs[i], o);

    /* System should be functional again. */
    void *recover = kalloc();
    TEST_ASSERT(recover != NULL, "alloc works after OOM recovery");
    kfree(recover);

    /* --- Panic cases: visual check only --- */

    /* Uncomment one at a time to verify the panic message is
     * human-readable, then comment back out so tests can continue.
     *
     * Double free:
     *   void *df = kalloc(); kfree(df); kfree(df);
     *
     * Order mismatch on free:
     *   void *om = kalloc_pages(2); kfree_pages(om, 1);
     *
     * Free with wrong alignment:
     *   void *wa = kalloc(); kfree((void *)((uint64)wa + 1));
     *
     * Free address below free_start:
     *   kfree((void *)KERN_BASE);
     */
}
