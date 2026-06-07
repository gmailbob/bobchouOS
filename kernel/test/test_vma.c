/*
 * test_vma.c — Unit tests for VMA operations (Round 6-3).
 *
 * Tests vma_create, vma_add (sorted insert), vma_find, and vma_free_all.
 * vma_dup_all is an integration test (requires two page tables + pages)
 * and is exercised by the fork path in `make run`.
 */

#include "test/test.h"
#include "proc.h"
#include "vma.h"
#include "kalloc.h"
#include "vm.h"
#include "string.h"

void
test_vma(void) {
    kprintf("[test_vma]\n");

    /* --- vma_create --- */
    struct vma *v = vma_create(0x1000, 0x3000, PTE_R | PTE_X | PTE_U);
    TEST_ASSERT(v != NULL, "vma_create: returns non-NULL");
    TEST_ASSERT(v->start == 0x1000, "vma_create: start = 0x1000");
    TEST_ASSERT(v->end == 0x3000, "vma_create: end = 0x3000");
    TEST_ASSERT(v->perm == (PTE_R | PTE_X | PTE_U), "vma_create: perm = RX|U");
    TEST_ASSERT(list_empty(&v->link), "vma_create: link self-pointing (not on list)");

    /* --- vma_add: sorted insertion --- */
    /* Set up a fake proc with just a vma_list (don't need a full proc) */
    struct proc fake;
    memset(&fake, 0, sizeof(fake));
    INIT_LIST_HEAD(&fake.vma_list);

    struct vma *v1 = vma_create(0x1000, 0x2000, PTE_R | PTE_U);
    struct vma *v2 = vma_create(0x5000, 0x6000, PTE_R | PTE_W | PTE_U);
    struct vma *v3 = vma_create(0x3000, 0x4000, PTE_R | PTE_U);

    vma_add(&fake, v2); /* [0x5000] */
    vma_add(&fake, v1); /* [0x1000, 0x5000] — inserted before v2 */
    vma_add(&fake, v3); /* [0x1000, 0x3000, 0x5000] — inserted between */

    /* Verify sorted order by walking the list */
    struct vma *pos;
    uint64 prev_start = 0;
    int count = 0;
    list_for_each_entry(pos, &fake.vma_list, link) {
        TEST_ASSERT(pos->start > prev_start, "vma_add: list is sorted");
        prev_start = pos->start;
        count++;
    }
    TEST_ASSERT(count == 3, "vma_add: all three VMAs present");

    /* --- vma_find --- */
    struct vma *found;

    found = vma_find(&fake, 0x1000); /* start of v1 */
    TEST_ASSERT(found == v1, "vma_find: finds v1 at start");

    found = vma_find(&fake, 0x1FFF); /* last byte of v1 */
    TEST_ASSERT(found == v1, "vma_find: finds v1 at end-1");

    found = vma_find(&fake, 0x3500); /* middle of v3 */
    TEST_ASSERT(found == v3, "vma_find: finds v3 in middle");

    found = vma_find(&fake, 0x2000); /* gap between v1 and v3 */
    TEST_ASSERT(found == NULL, "vma_find: returns NULL for unmapped gap");

    found = vma_find(&fake, 0x0000); /* below all VMAs */
    TEST_ASSERT(found == NULL, "vma_find: returns NULL below all");

    found = vma_find(&fake, 0x9000); /* above all VMAs */
    TEST_ASSERT(found == NULL, "vma_find: returns NULL above all");

    /* --- vma_free_all --- */
    /* Build a minimal proc with a page table + one VMA with a mapped page */
    struct proc fake2;
    memset(&fake2, 0, sizeof(fake2));
    INIT_LIST_HEAD(&fake2.vma_list);
    fake2.pagetable = (pte_t *)kalloc();
    memset(fake2.pagetable, 0, PG_SIZE);

    void *pg = kalloc();
    map_pages(fake2.pagetable, 0x1000, PG_SIZE, (uint64)pg, PTE_R | PTE_U);
    vma_add(&fake2, vma_create(0x1000, 0x2000, PTE_R | PTE_U));

    vma_free_all(&fake2);

    TEST_ASSERT(list_empty(&fake2.vma_list), "vma_free_all: list empty after free");
    /* The page was freed (page_put → kfree). The PTE should be cleared. */
    pte_t *pte = walk(fake2.pagetable, 0x1000, 0);
    TEST_ASSERT(pte == NULL || !(*pte & PTE_V), "vma_free_all: PTE cleared");

    /* Clean up the page table itself */
    proc_free_pagetable(fake2.pagetable);
}
