/*
 * test_vm.c — Tests for virtual memory (page table operations).
 */

#include "types.h"
#include "riscv.h"
#include "mem_layout.h"
#include "kalloc.h"
#include "vm.h"
#include "string.h"
#include "test/test.h"

void
test_vm(void) {
    kprintf("[test_vm]\n");

    /* --- pa_to_pte / pte_to_pa round-trip --- */

    uint64 pa = 0x80001000;
    pte_t pte = pa_to_pte(pa);
    TEST_ASSERT(pte_to_pa(pte) == pa, "pa_to_pte/pte_to_pa round-trip");

    TEST_ASSERT(pte_to_pa(pa_to_pte(0x80000000)) == 0x80000000, "round-trip KERN_BASE");
    TEST_ASSERT(pte_to_pa(pa_to_pte(PHYS_STOP)) == PHYS_STOP, "round-trip PHYS_STOP");

    /* --- PX macro extracts correct VPN indices --- */

    uint64 va = 0x80001234;
    TEST_ASSERT(PX(0, va) == 1, "PX(0, 0x80001234) == 1");
    TEST_ASSERT(PX(1, va) == 0, "PX(1, 0x80001234) == 0");
    TEST_ASSERT(PX(2, va) == 2, "PX(2, 0x80001234) == 2");

    /* --- pte_flags extracts low 10 bits --- */

    pte_t test_pte = pa_to_pte(0x80000000) | PTE_V | PTE_R | PTE_W;
    TEST_ASSERT(pte_flags(test_pte) == (PTE_V | PTE_R | PTE_W), "pte_flags extracts flags only");

    /* --- walk + map_pages on a fresh page table --- */

    pte_t *pt = (pte_t *)kalloc();
    TEST_ASSERT(pt != NULL, "kalloc for test page table");
    memset(pt, 0, PG_SIZE);

    /* Map one page: VA 0x1000 → PA 0x80010000, RW */
    int ret = map_pages(pt, 0x1000, PG_SIZE, 0x80010000, PTE_R | PTE_W);
    TEST_ASSERT(ret == 0, "map_pages returns 0 on success");

    /* walk should find the PTE we just installed */
    pte_t *found = walk(pt, 0x1000, 0);
    TEST_ASSERT(found != NULL, "walk finds mapped PTE");
    TEST_ASSERT((*found & PTE_V) != 0, "mapped PTE has V bit");
    TEST_ASSERT((*found & PTE_R) != 0, "mapped PTE has R bit");
    TEST_ASSERT((*found & PTE_W) != 0, "mapped PTE has W bit");
    TEST_ASSERT(pte_to_pa(*found) == 0x80010000, "mapped PTE points to correct PA");

    /* walk for a VA with no intermediate pages should return NULL (alloc=0).
     * 0x40000000 has VPN[2]=1 — no L1 table exists for it. */
    pte_t *missing = walk(pt, 0x40000000, 0);
    TEST_ASSERT(missing == NULL, "walk returns NULL for unmapped VA");

    /* walk with alloc=1 should create intermediate pages.
     * 0x80000000 has VPN[2]=2 — no L1/L0 tables exist for it yet. */
    pte_t *created = walk(pt, 0x80000000, 1);
    TEST_ASSERT(created != NULL, "walk(alloc=1) creates PTE");
    TEST_ASSERT((*created & PTE_V) == 0, "newly created PTE is not yet valid");

    /* --- paging should be on after vm_init_hart --- */

    uint64 satp_val = csrr(satp);
    TEST_ASSERT((satp_val >> 60) == 8, "satp MODE == Sv39 (8)");
    TEST_ASSERT((satp_val & 0xFFFFFFFFFFF) != 0, "satp PPN != 0");
}
