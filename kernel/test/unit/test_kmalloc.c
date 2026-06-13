/*
 * test_kmalloc.c -- Tests for the slab allocator (Round 4-2).
 */

#include "test/test.h"
#include "kalloc.h"
#include "kmalloc.h"
#include "riscv.h"
#include "types.h"

void
test_kmalloc(void) {
    kprintf("[kmalloc]\n");

    /* --- Basic allocation and free --- */

    void *p1 = kmalloc(100);
    TEST_ASSERT(p1 != NULL, "kmalloc(100) returns non-NULL");

    void *p2 = kmalloc(32);
    TEST_ASSERT(p2 != NULL, "kmalloc(32) returns non-NULL");
    TEST_ASSERT(p2 != p1, "different allocations return different pointers");

    kmfree(p1);
    kmfree(p2);

    /* --- Zero-size allocation --- */

    void *pz = kmalloc(0);
    TEST_ASSERT(pz == NULL, "kmalloc(0) returns NULL");

    /* --- Alignment: pointers should be aligned to at least the slot size --- */

    void *a32 = kmalloc(1);
    TEST_ASSERT((uint64)a32 % 32 == 0, "kmalloc(1) aligned to 32");
    kmfree(a32);

    void *a64 = kmalloc(33);
    TEST_ASSERT((uint64)a64 % 64 == 0, "kmalloc(33) aligned to 64");
    kmfree(a64);

    void *a128 = kmalloc(65);
    TEST_ASSERT((uint64)a128 % 128 == 0, "kmalloc(65) aligned to 128");
    kmfree(a128);

    /* --- Reuse after free --- */

    void *r1 = kmalloc(50);
    kmfree(r1);
    void *r2 = kmalloc(50);
    TEST_ASSERT(r2 == r1, "freed slot reused by same-class alloc");
    kmfree(r2);

    /* --- Multiple slabs: exhaust one slab of 32-byte class --- */

    int nr_slots = PG_SIZE / 32; /* 128 slots per slab */
    void *slots[128];
    int all_ok = 1;
    for (int i = 0; i < nr_slots; i++) {
        slots[i] = kmalloc(32);
        if (!slots[i]) {
            all_ok = 0;
            break;
        }
    }
    TEST_ASSERT(all_ok, "exhaust first 32B slab (128 allocs)");

    /* One more allocation should trigger a second slab. */
    void *extra = kmalloc(32);
    TEST_ASSERT(extra != NULL, "129th alloc triggers second slab");

    /* The extra pointer should be on a different page than slot[0]. */
    uint64 page1 = (uint64)slots[0] & ~(PG_SIZE - 1);
    uint64 page2 = (uint64)extra & ~(PG_SIZE - 1);
    TEST_ASSERT(page1 != page2, "second slab on different page");

    /* Clean up. */
    kmfree(extra);
    for (int i = 0; i < nr_slots; i++)
        kmfree(slots[i]);

    /* --- Big allocation path --- */

    void *big1 = kmalloc(3000);
    TEST_ASSERT(big1 != NULL, "kmalloc(3000) big-alloc returns non-NULL");
    TEST_ASSERT((uint64)big1 % PG_SIZE == 0, "big-alloc is page-aligned");

    struct page *big1_pg = pa_to_page((uint64)big1);
    TEST_ASSERT(big1_pg->flags & PG_BIG, "big-alloc page has PG_BIG flag");
    kmfree(big1);

    /* Multi-page big alloc. */
    void *big2 = kmalloc(8192);
    TEST_ASSERT(big2 != NULL, "kmalloc(8192) returns non-NULL");
    TEST_ASSERT((uint64)big2 % PG_SIZE == 0, "8K big-alloc page-aligned");

    struct page *big2_pg = pa_to_page((uint64)big2);
    TEST_ASSERT(big2_pg->flags & PG_BIG, "8K big-alloc has PG_BIG flag");
    TEST_ASSERT(big2_pg->order >= 1, "8K big-alloc order >= 1");
    kmfree(big2);

    /* --- Slab reclamation: second empty slab is freed --- */

    /* Allocate from two slabs of 64-byte class. */
    int nr_64 = PG_SIZE / 64; /* 64 slots */
    void *slab1_ptrs[64];
    for (int i = 0; i < nr_64; i++)
        slab1_ptrs[i] = kmalloc(64);
    void *slab2_ptr = kmalloc(64); /* triggers second slab */
    TEST_ASSERT(slab2_ptr != NULL, "second 64B slab allocated");

    /* Free the second slab's only allocation. The class now has two
     * slabs: the first is full, the second is empty. The empty second
     * slab should be freed back to the buddy allocator. */
    kmfree(slab2_ptr);

    /* Clean up first slab. */
    for (int i = 0; i < nr_64; i++)
        kmfree(slab1_ptrs[i]);

    /* --- One-slab-kept: sole empty slab is not freed --- */

    /* Allocate and free from 256-byte class. After freeing, the class
     * should still have one slab (not freed). */
    void *keep = kmalloc(200);
    TEST_ASSERT(keep != NULL, "kmalloc(200) non-NULL");
    struct page *keep_pg = pa_to_page((uint64)keep);
    uint8 keep_class = keep_pg->slab.class_idx;
    kmfree(keep);
    /* The slab should still be alive (nr_alloc == 0 but not freed). */
    TEST_ASSERT(keep_pg->slab.class_idx == keep_class, "sole empty slab kept alive");

    /* --- Exact class boundaries (32, 64, 128, 256, 512, 1024, 2048) --- */

    void *b32 = kmalloc(32);
    TEST_ASSERT(b32 != NULL, "kmalloc(32) exact boundary non-NULL");
    TEST_ASSERT((uint64)b32 % 32 == 0, "kmalloc(32) aligned to 32");
    kmfree(b32);

    void *b64 = kmalloc(64);
    TEST_ASSERT(b64 != NULL, "kmalloc(64) exact boundary non-NULL");
    TEST_ASSERT((uint64)b64 % 64 == 0, "kmalloc(64) aligned to 64");
    kmfree(b64);

    void *b128 = kmalloc(128);
    TEST_ASSERT(b128 != NULL, "kmalloc(128) exact boundary non-NULL");
    TEST_ASSERT((uint64)b128 % 128 == 0, "kmalloc(128) aligned to 128");
    kmfree(b128);

    void *b256 = kmalloc(256);
    TEST_ASSERT(b256 != NULL, "kmalloc(256) exact boundary non-NULL");
    TEST_ASSERT((uint64)b256 % 256 == 0, "kmalloc(256) aligned to 256");
    kmfree(b256);

    void *b512 = kmalloc(512);
    TEST_ASSERT(b512 != NULL, "kmalloc(512) exact boundary non-NULL");
    TEST_ASSERT((uint64)b512 % 512 == 0, "kmalloc(512) aligned to 512");
    kmfree(b512);

    void *b1024 = kmalloc(1024);
    TEST_ASSERT(b1024 != NULL, "kmalloc(1024) exact boundary non-NULL");
    TEST_ASSERT((uint64)b1024 % 1024 == 0, "kmalloc(1024) aligned to 1024");
    kmfree(b1024);

    void *b2048 = kmalloc(2048);
    TEST_ASSERT(b2048 != NULL, "kmalloc(2048) exact boundary non-NULL");
    TEST_ASSERT((uint64)b2048 % 2048 == 0, "kmalloc(2048) aligned to 2048");
    kmfree(b2048);

    /* Boundary between slab and big-alloc: PG_SIZE is the first big-alloc */
    void *bpg = kmalloc(PG_SIZE);
    TEST_ASSERT(bpg != NULL, "kmalloc(PG_SIZE) big-alloc non-NULL");
    TEST_ASSERT((uint64)bpg % PG_SIZE == 0, "kmalloc(PG_SIZE) page-aligned");
    struct page *bpg_pg = pa_to_page((uint64)bpg);
    TEST_ASSERT(bpg_pg->flags & PG_BIG, "kmalloc(PG_SIZE) has PG_BIG flag");
    kmfree(bpg);
}
