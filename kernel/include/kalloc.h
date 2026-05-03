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

#define BIG_ALLOC 0xFF /* sentinel: page is a kmalloc big allocation */

/*
 * Per-page metadata. One entry per physical page in the system.
 *
 * - refcount: always valid regardless of page state.
 * - order: buddy block order (0 = single page). Lives outside the
 *   union because buddy merging needs it regardless of page role.
 * - union: role-specific fields. A page is only one thing at a time
 *   (free, slab, page table, user page), so the fields overlay.
 */
struct page {
    uint16 refcount;
    uint8 order;
    union {
        struct {
            uint8 class_idx;        /* index into size_classes[] or BIG_ALLOC */
            uint16 nr_alloc;        /* currently allocated slots */
            void *free_list;        /* first free slot in this slab */
            struct page *next_slab; /* next slab in class's list */
        } slab;
    };
};

void kalloc_init(void);
void *kalloc(void);
void kfree(void *pa);
void *kalloc_pages(int order);
void kfree_pages(void *pa, int order);

struct page *pa_to_page(uint64 pa);

#endif /* KALLOC_H */
