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

static void
print_allocator(const char *s) {
    kprintf("%s | buddy allocator stats:\n", s);
    for (int i = 0; i <= MAX_ORDER; i++) {
        kprintf("  order %d: nr_free=%d, free_list=", i, free_areas[i].nr_free);
        struct run *r = free_areas[i].free_list;
        while (r) {
            kprintf("%p -> ", r);
            r = r->next;
        }
        kprintf("NULL\n");
    }
}

/*
 * buddy_alloc — Allocate a contiguous block of 2^order pages.
 *
 * Search for a free block at the requested order. If none is
 * available, find a larger block and split it repeatedly until
 * a block of the right order is produced. The "buddy" (the upper
 * half of each split) goes onto the appropriate free list.
 *
 * Set page->order on the allocated block's first page.
 * Set page->refcount = 1 on the first page.
 * Zero the entire block.
 * Returns NULL if no block can be found at any order.
 */
static void *
buddy_alloc(uint32 order) {
    if (order > MAX_ORDER)
        panic("buddy_alloc: order=%d", order);

    uint32 init_order = order;
    struct run *r;
    while (!(r = free_areas[order].free_list)) {
        if (++order > MAX_ORDER)
            return NULL;
    }

    // release one area from order
    free_areas[order].free_list = r->next;
    free_areas[order].nr_free--;
    // for each in [init_order, order) set to higher half
    for (uint32 i = init_order; i < order; i++) {
        struct run *buddy = (struct run *)((uint64)r ^ (PG_SIZE << i));
        // at present buddy is junk; set first 8 bytes to NULL as tail of free list
        buddy->next = NULL;
        free_areas[i].free_list = buddy;
        free_areas[i].nr_free = 1;
    }

    memset(r, 0, PG_SIZE << init_order);
    struct page *pg = pa_to_page((uint64)r);
    pg->order = init_order;
    pg->refcount = 1;
    return r;
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
 */
static void
buddy_free(void *pa, uint32 order) {
    uint64 addr = (uint64)pa;
    if (order > MAX_ORDER || addr % (PG_SIZE << order) || addr < free_start || addr >= PHYS_STOP)
        panic("buddy_free: invalid physical address=%p order=%d free_start=%p", pa, order,
              free_start);

    struct page *pg = pa_to_page(addr);
    if (pg->refcount != 1 || pg->order != order)
        panic("buddy_free: pa=%p order=%d; pg->refcount=%d, pg->order=%d", pa, order, pg->refcount,
              pg->order);

    memset(pa, 1, PG_SIZE << order);
    pg->refcount = 0;

    while (order < MAX_ORDER) {
        uint64 buddy = addr ^ (PG_SIZE << order);
        if (pa_to_page(buddy)->refcount) // buddy not free, break
            break;

        struct run **pr = &free_areas[order].free_list;
        while (*pr && *pr != (struct run *)buddy)
            pr = &(*pr)->next;
        if (*pr == NULL)
            break; // buddy not same order, break

        *pr = (*pr)->next;
        free_areas[order].nr_free--;
        addr &= ~(PG_SIZE << order++); // set addr to lower half and continue with order + 1
    }

    struct run *r = (void *)addr;
    r->next = free_areas[order].free_list;
    free_areas[order].free_list = r;
    free_areas[order].nr_free++;
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
 */
void
kalloc_init(void) {
    nr_pages = (PHYS_STOP - KERN_BASE) >> PG_SHIFT;
    pages = (struct page *)PG_ROUND_UP((uint64)_kernel_end);

    uint64 size = nr_pages * sizeof(struct page);
    memset(pages, 0, size);
    free_start = PG_ROUND_UP((uint64)pages + size);

    uint64 addr = free_start;
    while (addr < PHYS_STOP) {
        uint32 order = 1;
        while (addr + (PG_SIZE << order) < PHYS_STOP && addr % (PG_SIZE << order) == 0 &&
               ++order <= MAX_ORDER)
            ;
        kprintf("[debug] kalloc_init calls buddy_free(addr=%p, order=%d)\n", addr, --order);
        // mimic refcount and order, otherwise panic
        pa_to_page(addr)->refcount = 1;
        pa_to_page(addr)->order = order;
        buddy_free((void *)addr, order);
        addr += PG_SIZE << order;
    }

    print_allocator("kalloc_init complete");
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
kalloc_pages(uint32 order) {
    if (order > MAX_ORDER)
        panic("kalloc_pages: invalid order %d", (int)order);
    return buddy_alloc(order);
}

/* Free a multi-page block of 2^order pages. */
void
kfree_pages(void *pa, uint32 order) {
    if (order > MAX_ORDER)
        panic("kfree_pages: invalid order %d", (int)order);
    buddy_free(pa, order);
}
