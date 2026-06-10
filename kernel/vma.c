/*
 * vma.c — Virtual Memory Area (VMA) operations.
 *
 * A VMA describes one contiguous, page-aligned region of a process's
 * user address space (text, data, stack, ...) together with its intended
 * permissions. Each process owns a list of VMAs (proc->vma_list), kept
 * sorted by start address. The VMA list — not a flat "size" field — is the
 * authoritative record of which user pages a process owns; fork copies it,
 * exec rebuilds it, and exit frees it.
 *
 * Operations:
 *   vma_create   — allocate one descriptor
 *   vma_add      — insert into a process's list, keeping it sorted
 *   vma_find     — locate the VMA containing an address
 *   vma_dup_all  — deep-copy a process's whole address space (fork)
 *   vma_free_all — free a process's whole address space (exec/exit)
 *
 * Physical pages are reference-counted (page_get/page_put). COW fork
 * shares pages (refcount > 1); page_put frees only when the last
 * reference is dropped.
 *
 * See Lectures 6-3 (Parts 1 and 3) and 6-4.
 */

#include "vma.h"
#include "kalloc.h"
#include "kmalloc.h"
#include "proc.h"
#include "vm.h"
#include "string.h"

/*
 * vma_create — allocate and initialize a VMA descriptor.
 *
 * Returns NULL on allocation failure.
 */
struct vma *
vma_create(uint64 start, uint64 end, int perm, uint32 flags) {
    struct vma *v = kmalloc(sizeof(struct vma));
    if (!v)
        return NULL;
    v->start = start;
    v->end = end;
    v->perm = perm;
    v->flags = flags;
    INIT_LIST_HEAD(&v->link);
    return v;
}

/*
 * vma_add — insert a VMA into the process's list, sorted by start address.
 *
 * Keeping the list sorted lets vma_find / overlap checks stop early, and
 * mirrors how real kernels keep VMAs ordered (Linux uses an rb-tree).
 */
void
vma_add(struct proc *p, struct vma *v) {
    /* Default: append at the tail (list head is the "one past last" point). */
    struct list_head *insert_point = &p->vma_list;
    struct vma *pos;
    list_for_each_entry(pos, &p->vma_list, link) {
        /* Insert before the first VMA that starts after us. */
        if (v->start < pos->start) {
            insert_point = &pos->link;
            break;
        }
    }
    /* list_add_tail(node, head) inserts node just before head — so passing
     * pos->link inserts before pos, and passing &p->vma_list appends. */
    list_add_tail(&v->link, insert_point);
}

/*
 * vma_find — return the VMA containing addr, or NULL if addr is unmapped.
 *
 * This is the "is this address valid?" query that replaces the flat
 * "addr < sz" check. A future page-fault handler (COW, lazy alloc) will
 * use it to decide whether a faulting address is legal.
 */
struct vma *
vma_find(struct proc *p, uint64 addr) {
    struct vma *pos;
    list_for_each_entry(pos, &p->vma_list, link) {
        if (pos->start <= addr && addr < pos->end) /* [start, end) */
            return pos;
    }
    return NULL;
}

/*
 * vma_find_by_flags — return the first VMA with matching flags, or NULL.
 */
struct vma *
vma_find_by_flags(struct proc *p, uint32 flags) {
    struct vma *pos;
    list_for_each_entry(pos, &p->vma_list, link) {
        if (pos->flags & flags)
            return pos;
    }
    return NULL;
}

/*
 * vma_dup_all — COW-duplicate the address space from src to dst.
 *
 * Used by fork. For each VMA in src: create a matching VMA in dst, then
 * share physical pages via copy-on-write (no allocation, no memcpy).
 *
 * COW protocol per page:
 *   - Skip pages with no valid PTE (lazy — not yet faulted in)
 *   - If writable: clear PTE_W, set PTE_COW in BOTH parent and child PTEs
 *   - Map same physical page in dst with the (possibly downgraded) flags
 *   - page_get to increment refcount (1 → 2)
 *   - After all pages: sfence_vma to flush parent's stale TLB entries
 *
 * Returns 0 on success, -1 on failure. On failure dst is left clean —
 * vma_free_all(dst) reclaims everything mapped so far.
 *
 * Note: only user VMAs are duplicated. The trampoline and trapframe are
 * kernel-managed mappings installed by proc_pagetable, not VMAs.
 */
int
vma_dup_all(struct proc *dst, struct proc *src) {
    struct vma *pos;
    list_for_each_entry(pos, &src->vma_list, link) {
        struct vma *nv = vma_create(pos->start, pos->end, pos->perm, pos->flags);
        if (!nv)
            goto fail;
        vma_add(dst, nv);

        for (uint64 va = pos->start; va < pos->end; va += PG_SIZE) {
            pte_t *pte = walk(src->pagetable, va, 0);
            if (!pte || !(*pte & PTE_V))
                continue; /* lazy page — not faulted in yet, skip */

            uint64 pa = pte_to_pa(*pte);
            if (*pte & PTE_W)
                *pte = (*pte & ~PTE_W) | PTE_COW;
            if (map_pages(dst->pagetable, va, PG_SIZE, pa, (int)pte_flags(*pte)) < 0)
                goto fail;
            page_get((void *)pa);
        }
    }

    sfence_vma();
    return 0;

fail:
    /* All committed VMAs (including the in-progress one, already added)
     * are on dst->vma_list, so this frees every page we mapped. */
    vma_free_all(dst);
    return -1;
}

/*
 * vma_free_all — free every VMA and its mapped user pages.
 *
 * Used by exec (to discard the old address space) and exit (final cleanup).
 * For each VMA: walk its page range, page_put each mapped page (freeing it
 * when refcount hits 0), clear the leaf PTE, then free the VMA descriptor.
 *
 * Frees only leaf USER pages. The page-table's own intermediate pages, and
 * the trampoline/trapframe mappings, are freed separately by
 * proc_free_pagetable — so this must run BEFORE proc_free_pagetable (the
 * page table must still be walkable here). Uses list_for_each_entry_safe
 * because the loop frees each node as it goes.
 */
void
vma_free_all(struct proc *p) {
    struct vma *pos, *tmp;
    list_for_each_entry_safe(pos, tmp, &p->vma_list, link) {
        for (uint64 va = pos->start; va < pos->end; va += PG_SIZE) {
            pte_t *pte = walk(p->pagetable, va, 0);
            if (!pte || !(*pte & PTE_V)) /* skip holes (e.g. guard pages) */
                continue;
            page_put((void *)pte_to_pa(*pte));
            *pte = 0; /* clear so proc_free_pagetable won't see a stale leaf */
        }
        list_del(&pos->link);
        kmfree(pos);
    }
}
