/*
 * kalloc.h — Physical page allocator interface.
 *
 * Manages 4 KB physical pages with a free list and a per-page
 * metadata array (struct page). See Lecture 3-2 for design rationale.
 */

#ifndef KALLOC_H
#define KALLOC_H

#include "types.h"

/* Per-page metadata. One entry per physical page in the system.
 * Currently only holds a refcount for COW fork support (Phase 6).
 * Additional fields (flags, LRU pointers) can be added later. */
struct page {
    uint16 refcount;
};

void kinit(void);
void *kalloc(void);
void kfree(void *pa);

struct page *pa_to_page(uint64 pa);

#endif /* KALLOC_H */
