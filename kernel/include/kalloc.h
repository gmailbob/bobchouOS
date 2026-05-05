/*
 * kalloc.h — Physical page allocator interface (buddy allocator).
 *
 * Manages physical pages using a buddy allocator with power-of-two
 * block sizes (orders 0-10). A struct page array provides per-page
 * metadata with a union for different page uses (slab, page table,
 * user page, etc.).
 *
 * See Lectures 3-2 and 4-2 for design rationale.
 */

#ifndef KALLOC_H
#define KALLOC_H

#include "types.h"

#define MAX_ORDER 10 /* max block = 2^10 = 1024 pages = 4 MB */

/* Page flags — identify how a page is being used. */
#define PG_SLAB (1 << 0) /* page is a slab (sub-page allocator) */
#define PG_BIG (1 << 1)  /* page is a big kmalloc allocation */

/*
 * Per-page metadata. One entry per physical page in the system.
 *
 * Ownership:
 *   refcount, order — managed by the buddy allocator (kalloc.c).
 *                     Consumers must not modify these.
 *   flags, union    — managed by the consumer (e.g., kmalloc.c).
 *                     Set flags on alloc, clear on free.
 *
 * - refcount: reference count. 0 = free, >=1 = allocated.
 * - order: buddy block order (0 = single page). Needed for merging.
 * - flags: identifies page role (PG_SLAB, PG_BIG, etc.).
 * - union: role-specific fields. A page is only one thing at a time
 *   (free, slab, page table, user page), so the fields overlay.
 */
struct page {
    uint16 refcount;
    uint8 order;
    uint8 flags;
    union {
        struct {
            uint8 class_idx;        /* index into size_classes[] (0-6) */
            uint16 nr_alloc;        /* currently allocated slots */
            void *free_list;        /* first free slot in this slab */
            struct page *next_slab; /* next slab in class's list */
        } slab;
    };
};

void kalloc_init(void);
void *kalloc(void);
void kfree(void *pa);
void *kalloc_pages(uint32 order);
void kfree_pages(void *pa, uint32 order);

struct page *pa_to_page(uint64 pa);
uint64 page_to_pa(struct page *pg);

#endif /* KALLOC_H */
