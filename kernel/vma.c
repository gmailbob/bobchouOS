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
    /* TODO(student): kmalloc a struct vma, initialize fields, return it */
    (void)start;
    (void)end;
    (void)perm;
    return 0;
}

/*
 * vma_add — insert a VMA into the process's list, sorted by start address.
 */
void
vma_add(struct proc *p, struct vma *v) {
    /* TODO(student): walk p->vma_list, insert v in sorted order */
    (void)p;
    (void)v;
}

/*
 * vma_find — find the VMA containing the given virtual address.
 *
 * Returns NULL if addr is not within any VMA.
 */
struct vma *
vma_find(struct proc *p, uint64 addr) {
    /* TODO(student): linear scan of p->vma_list, return vma where start <= addr < end */
    (void)p;
    (void)addr;
    return 0;
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
    /* TODO(student): iterate src->vma_list, copy each region page-by-page */
    (void)dst;
    (void)src;
    return -1;
}

/*
 * vma_free_all — free all VMAs and their mapped user pages.
 *
 * Walks each VMA's range, frees every valid user page via page_put,
 * clears PTEs, frees the VMA struct.
 */
void
vma_free_all(struct proc *p) {
    /* TODO(student): iterate p->vma_list with list_for_each_entry_safe,
     * walk pages, page_put each, clear PTE, kmfree the vma */
    (void)p;
}
