/*
 * vm_fault.c — Page fault handling: COW, lazy allocation, demand paging.
 *
 * The unified page fault handler dispatches to either:
 *   - lazy_alloc: VMA exists but no PTE → allocate a zeroed page
 *   - cow_copy:   PTE has PTE_COW → copy the shared page (or just flip bits)
 *
 * Called from user_trap when scause is a load/store page fault.
 *
 * See Lecture 6-4, Parts 5 and 6.
 */

#include "types.h"
#include "proc.h"
#include "vma.h"
#include "vm.h"
#include "kalloc.h"
#include "riscv.h"
#include "string.h"
#include "mem_layout.h"

/*
 * lazy_alloc — allocate a zeroed page for a valid but unmapped address.
 *
 * Called when a VMA covers the faulting address but no PTE exists (or PTE_V
 * is clear). Allocates a fresh zeroed page and maps it with the VMA's
 * intended permissions.
 *
 * Returns 0 on success, -1 on OOM.
 */
static int
lazy_alloc(struct proc *p, uint64 va, struct vma *vma) {
    void *pg = kalloc();
    if (!pg)
        return -1;

    if (map_pages(p->pagetable, PG_ROUND_DOWN(va), PG_SIZE, (uint64)pg, vma->perm) < 0) {
        kfree(pg);
        return -1;
    }

    return 0;
}

/*
 * cow_copy — handle a write fault on a copy-on-write page.
 *
 * Two cases:
 *   - refcount == 1: sole owner, just restore PTE_W and clear PTE_COW
 *   - refcount > 1:  allocate new page, memcpy, remap, page_put old
 *
 * Returns 0 on success, -1 on OOM.
 */
static int
cow_copy(pte_t *pte) {
    uint64 pa = pte_to_pa(*pte);
    if (pa_to_page(pa)->refcount == 1) {
        *pte = (*pte & ~PTE_COW) | PTE_W;
        sfence_vma();
        return 0;
    }

    void *pg = kalloc();
    if (!pg)
        return -1;

    memcpy(pg, (void *)pa, PG_SIZE);
    /* *pte & (PTE_COW - 1): keeps bits[0..7] */
    *pte = pa_to_pte((uint64)pg) | (*pte & (PTE_COW - 1)) | PTE_W;
    sfence_vma();
    page_put((void *)pa);

    return 0;
}

/*
 * handle_page_fault — unified page fault resolution.
 *
 * Called from two sites:
 *   - user_trap: hardware page fault (scause 13/15)
 *   - copyout/copyin: kernel pre-faults lazy/COW pages before memcpy
 *
 * Decision tree:
 *   1. vma_find(va) → NULL? → segfault (-1)
 *   2. walk(va) → no valid PTE?
 *      - store to non-writable VMA → segfault (-1)
 *      - else → lazy_alloc
 *   3. store fault + PTE_COW? → cow_copy
 *   4. else → genuine violation (-1)
 *
 * Returns 0 on success (fault resolved), -1 on failure.
 */
int
handle_page_fault(struct proc *p, uint64 cause, uint64 va) {
    struct vma *v = vma_find(p, va);
    if (!v)
        return -1;

    pte_t *pte = walk(p->pagetable, va, 0);

    if (!pte || !(*pte & PTE_V)) {
        if (cause == EXC_STORE_PAGE && !(v->perm & PTE_W))
            return -1;
        lazy_alloc(p, va, v);
        return 0;
    }

    if (cause == EXC_STORE_PAGE && (*pte & PTE_COW)) {
        cow_copy(pte);
        return 0;
    }

    return -1;
}
