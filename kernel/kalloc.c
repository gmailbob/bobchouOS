/*
 * kalloc.c — Buddy allocator for bobchouOS.
 *
 * Replaces the Phase 3 flat free list with a buddy allocator that
 * supports multi-page contiguous allocations. Pages are managed in
 * power-of-two blocks (orders 0 through MAX_ORDER). The struct page
 * array provides per-page metadata.
 *
 * See Lecture 4-2 for the full design discussion.
 */

#include "types.h"
#include "riscv.h"
#include "mem_layout.h"
#include "string.h"
#include "kprintf.h"
#include "kalloc.h"

extern char _kernel_end[];

/* Free block link — threaded through the first 8 bytes of free blocks. */
struct run {
    struct run *next;
};

/* Per-order free list. */
struct free_area {
    struct run *free_list;
    uint64 nr_free;
};

static struct page *pages;
static uint64 nr_pages;
static struct free_area free_areas[MAX_ORDER + 1];
static uint64 free_start;

/* Convert a physical address to its struct page entry. */
struct page *
pa_to_page(uint64 pa) {
    return &pages[(pa - KERN_BASE) >> PG_SHIFT];
}

/* Convert a struct page entry back to its physical address. */
static uint64
page_to_pa(struct page *pg) {
    return KERN_BASE + ((uint64)(pg - pages) << PG_SHIFT);
}

/*
 * buddy_alloc — Allocate a contiguous block of 2^order pages.
 *
 * Search for a free block at the requested order. If none is
 * available, find a larger block and split it repeatedly until
 * a block of the right order is produced. The "buddy" (the other
 * half of each split) goes onto the appropriate free list.
 *
 * Set page->order on the allocated block's first page.
 * Set page->refcount = 1 on the first page.
 * Zero the entire block.
 * Returns NULL if no block can be found at any order.
 *
 * TODO: Implement buddy_alloc.
 */
static void *
buddy_alloc(int order) {
    /* TODO */
    (void)order;
    return NULL;
}

/*
 * buddy_free — Free a block of 2^order pages starting at pa.
 *
 * Validate the address and alignment. Junk-fill the block with 0x01.
 * Set refcount = 0 on the first page.
 *
 * Then attempt to merge with the buddy:
 *   1. Compute buddy address: pa XOR (PG_SIZE << order).
 *   2. Check if buddy is free (refcount == 0) and at the same order.
 *   3. If so, remove buddy from its free list, merge into a block
 *      of order+1 (the combined block starts at the lower address).
 *   4. Repeat until buddy is in-use or order reaches MAX_ORDER.
 *   5. Place the (possibly merged) block on the appropriate free list.
 *
 * TODO: Implement buddy_free.
 */
static void
buddy_free(void *pa, int order) {
    /* TODO */
    (void)pa;
    (void)order;
}

/*
 * kalloc_init — Initialize the buddy allocator.
 *
 * 1. Compute nr_pages = (PHYS_STOP - KERN_BASE) / PG_SIZE.
 * 2. Place the struct page array right after _kernel_end (page-aligned).
 * 3. Zero the entire struct page array.
 * 4. Compute free_start: first page after the struct page array.
 * 5. Initialize all free_areas to empty.
 * 6. Add all free pages [free_start, PHYS_STOP) to the buddy system.
 *    For each contiguous range, find the largest power-of-two block
 *    that fits at each aligned address, and add it via buddy_free.
 *    (Hint: start from free_start, find the highest order whose
 *    block size fits and whose alignment requirement is met, add it,
 *    advance, repeat.)
 * 7. Print diagnostic: "kalloc_init: buddy allocator, orders 0-10,
 *    N free pages (N MB)"
 *
 * TODO: Implement kalloc_init.
 */
void
kalloc_init(void) {
    /* TODO */
}

/* Allocate one zeroed 4 KB page (buddy order 0). */
void *
kalloc(void) {
    return buddy_alloc(0);
}

/* Free a single page. */
void
kfree(void *pa) {
    buddy_free(pa, 0);
}

/* Allocate 2^order contiguous pages. */
void *
kalloc_pages(int order) {
    if (order < 0 || order > MAX_ORDER)
        panic("kalloc_pages: invalid order %d", order);
    return buddy_alloc(order);
}

/* Free a multi-page block of 2^order pages. */
void
kfree_pages(void *pa, int order) {
    if (order < 0 || order > MAX_ORDER)
        panic("kfree_pages: invalid order %d", order);
    buddy_free(pa, order);
}
