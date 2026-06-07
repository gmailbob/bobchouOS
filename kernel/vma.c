/*
 * vma.c — Virtual Memory Area operations.
 *
 * Manages per-process VMA lists: create, add (sorted), find by address,
 * duplicate all (for fork), free all (for exec/exit).
 *
 * See Lecture 6-3, Parts 1 and 3.
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
vma_create(uint64 start, uint64 end, int perm) {
    struct vma *pvma = (struct vma *)kmalloc(sizeof(struct vma));
    if (!pvma)
        return NULL;
    pvma->start = start;
    pvma->end = end;
    pvma->perm = perm;
    INIT_LIST_HEAD(&pvma->link);
    return pvma;
}

/*
 * vma_add — insert a VMA into the process's list, sorted by start address.
 */
void
vma_add(struct proc *p, struct vma *v) {
    struct list_head *insert_point = &p->vma_list;
    struct vma *pos;
    list_for_each_entry(pos, &p->vma_list, link) {
        if (v->start > pos->start) {
            insert_point = &pos->link;
            break;
        }
    }
    list_add_tail(insert_point, &v->link);
}

/*
 * vma_find — find the VMA containing the given virtual address.
 *
 * Returns NULL if addr is not within any VMA.
 */
struct vma *
vma_find(struct proc *p, uint64 addr) {
    struct vma *pos;
    list_for_each_entry(pos, &p->vma_list, link) {
        if (pos->start <= addr && addr < pos->end)
            return pos;
    }
    return NULL;
}

/*
 * vma_dup_all — deep-copy all VMAs and their pages from src to dst.
 *
 * For each VMA in src: create a matching VMA in dst, allocate fresh
 * pages, memcpy content, map into dst's page table.
 *
 * Returns 0 on success, -1 on failure (dst is cleaned up on failure).
 */
int
vma_dup_all(struct proc *dst, struct proc *src) {
    /* iterate src->vma_list, copy each region page-by-page */
    struct vma *pos;
    list_for_each_entry(pos, &src->vma_list, link) {
        for (uint64 va = pos->start; va < pos->end; va += PG_SIZE) {
            pte_t *pte = walk(src->pagetable, va, 0);
            uint64 pa = pte_to_pa(*pte);
            void *pg = kalloc();
            memcpy(pg, (void *)pa, PG_SIZE);
            map_pages(dst->pagetable, va, PG_SIZE, pa, pos->perm);
        }
        list_add_tail(&vma_create(pos->start, pos->end, pos->perm)->link, &dst->vma_list);
    }
    return 0;
}

/*
 * vma_free_all — free all VMAs and their mapped user pages.
 *
 * Walks each VMA's range, frees every valid user page via page_put,
 * clears PTEs, frees the VMA struct.
 */
void
vma_free_all(struct proc *p) {
    struct vma *pos, *tmp;
    list_for_each_entry_safe(pos, tmp, &p->vma_list, link) {
        for (uint64 va = pos->start; va < pos->end; va += PG_SIZE) {
            pte_t *pte = walk(p->pagetable, va, 0);
            uint64 pa = pte_to_pa(*pte);
            page_put((void *)pa);
            *pte = 0;
        }
        list_del(&pos->link);
        kmfree(pos);
    }
}
