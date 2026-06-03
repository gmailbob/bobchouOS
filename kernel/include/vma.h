/*
 * vma.h — Virtual Memory Area management.
 *
 * Each process owns a sorted list of VMAs describing its user-accessible
 * address space regions. VMAs track intended permissions (which may differ
 * from current PTE permissions during COW or lazy allocation).
 *
 * See Lecture 6-3, Part 1.
 */

#ifndef VMA_H
#define VMA_H

#include "types.h"
#include "list.h"

struct proc;

struct vma {
    uint64 start;          /* first byte (page-aligned) */
    uint64 end;            /* one past last byte (page-aligned) */
    int perm;              /* intended permissions: PTE_R | PTE_W | PTE_X | PTE_U */
    struct list_head link; /* node in proc->vma_list */
};

struct vma *vma_create(uint64 start, uint64 end, int perm);
void vma_add(struct proc *p, struct vma *v);
struct vma *vma_find(struct proc *p, uint64 addr);
int vma_dup_all(struct proc *dst, struct proc *src);
void vma_free_all(struct proc *p);

#endif /* VMA_H */
