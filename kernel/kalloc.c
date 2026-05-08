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

/* Link for the buddy free list — overlaid on the first 8 bytes of a free block. */
struct block {
    struct block *next;
};

/* Per-order free list. */
struct free_area {
    struct block *free_list;
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
uint64
page_to_pa(struct page *pg) {
    return KERN_BASE + ((uint64)(pg - pages) << PG_SHIFT);
}

/* Print per-order free list stats. Useful for debugging after init. */
static void
buddy_dump(void) {
    for (uint32 i = 0; i <= MAX_ORDER; i++) {
        kprintf("  order %d: %d blocks (%d KB each), head=%p\n", i, (int)free_areas[i].nr_free,
                (int)((PG_SIZE << i) / 1024), free_areas[i].free_list);
    }
}

/*
 * buddy_alloc — Allocate a contiguous block of 2^order pages.
 *
 * Search for a free block at the requested order. If none is
 * available, find a larger block and split it repeatedly until
 * a block of the right order is produced. The upper half of each
 * split (the "buddy") goes onto the appropriate free list.
 *
 * Set page->order and page->refcount = 1 on the allocated block.
 * Zero the entire block. Return NULL if out of memory.
 */
static void *
buddy_alloc(uint32 order) {
    if (order > MAX_ORDER)
        panic("buddy_alloc: order=%d", order);

    /* Search upward for the smallest available block >= requested order. */
    uint32 found_order = order;
    struct block *b;
    while (!(b = free_areas[found_order].free_list)) {
        if (++found_order > MAX_ORDER)
            return NULL;
    }

    /* Remove the block from its free list. */
    free_areas[found_order].free_list = b->next;
    free_areas[found_order].nr_free--;

    /* Split down: for each level between the found order and the
     * requested order, put the upper buddy on the free list.
     * These lists are guaranteed empty (we searched past them), so
     * we could just set next=NULL and nr_free=1 directly. We prepend
     * to the existing list instead to stay safe if multi-hart races
     * arrive later.
     * Also set page->order on each buddy: order is only meaningful
     * when refcount>0 (allocated), but setting it here avoids stale
     * values in debug dumps. */
    for (uint32 i = order; i < found_order; i++) {
        struct block *buddy = (struct block *)((uint64)b ^ (PG_SIZE << i));
        pa_to_page((uint64)buddy)->order = i;
        buddy->next = free_areas[i].free_list;
        free_areas[i].free_list = buddy;
        free_areas[i].nr_free++;
    }

    /* Zero on alloc: page table pages need all PTEs to start invalid (V=0).
     * A stale non-zero PTE with V set would create phantom mappings.
     * Linux's alloc_pages does NOT zero — callers use get_zeroed_page()
     * when needed. We always zero for safety since page tables are our
     * primary consumer. */
    memset(b, 0, PG_SIZE << order);
    struct page *pg = pa_to_page((uint64)b);
    pg->order = order;
    pg->refcount = 1;
    return b;
}

/*
 * buddy_free — Free a block of 2^order pages starting at pa.
 *
 * Validate address and alignment. Junk-fill the block with 0x01.
 * Set refcount = 0 on the first page.
 *
 * Then attempts to merge with the buddy:
 *   1. Compute buddy address: pa XOR (PG_SIZE << order).
 *   2. If buddy is free and on this order's free list, remove it,
 *      merge into a block of order+1 (starting at the lower address).
 *   3. Repeat until the buddy is in-use or order reaches MAX_ORDER.
 *   4. Place the (possibly merged) block on the appropriate free list.
 */
static void
buddy_free(void *pa, uint32 order) {
    uint64 addr = (uint64)pa;
    if (order > MAX_ORDER || addr % (PG_SIZE << order) || addr < free_start || addr >= PHYS_STOP)
        panic("buddy_free: invalid pa=%p order=%d", pa, order);

    struct page *pg = pa_to_page(addr);
    if (pg->refcount != 1 || pg->order != order)
        panic("buddy_free: pa=%p order=%d refcount=%d pg->order=%d", pa, order, pg->refcount,
              pg->order);

    memset(pa, 1, PG_SIZE << order);
    pg->refcount = 0;

    while (order < MAX_ORDER) {
        uint64 buddy = addr ^ (PG_SIZE << order);
        if (buddy < free_start || buddy >= PHYS_STOP ||
            pa_to_page(buddy)->refcount) /* buddy in use */
            break;

        /* Try to find buddy on this order's free list. */
        struct block **pb = &free_areas[order].free_list;
        while (*pb && *pb != (struct block *)buddy)
            pb = &(*pb)->next;
        if (*pb == NULL)
            break;

        /* Remove buddy and merge into the lower-addressed block. */
        *pb = (*pb)->next;
        free_areas[order].nr_free--;
        addr &= ~(PG_SIZE << order);
        order++;
    }

    struct block *b = (void *)addr;
    b->next = free_areas[order].free_list;
    free_areas[order].free_list = b;
    free_areas[order].nr_free++;
}

/*
 * kalloc_init — Initialize the buddy allocator.
 *
 * Builds the struct page array, then groups all free memory into the
 * largest possible buddy blocks. Uses buddy_free to add each block,
 * which requires setting refcount=1 and order first (buddy_free
 * validates these before freeing).
 */
void
kalloc_init(void) {
    nr_pages = (PHYS_STOP - KERN_BASE) >> PG_SHIFT;
    pages = (struct page *)PG_ROUND_UP((uint64)_kernel_end);

    uint64 array_size = nr_pages * sizeof(struct page);
    memset(pages, 0, array_size);
    free_start = PG_ROUND_UP((uint64)pages + array_size);

    /* Add free memory to the buddy system in the largest aligned blocks. */
    uint64 nr_total_free = 0;
    uint64 addr = free_start;
    while (addr < PHYS_STOP) {
        uint32 order = 0;
        while (order < MAX_ORDER && addr + (PG_SIZE << (order + 1)) <= PHYS_STOP &&
               addr % (PG_SIZE << (order + 1)) == 0)
            order++;

        /* buddy_free expects refcount=1 and order set on the page. */
        pa_to_page(addr)->refcount = 1;
        pa_to_page(addr)->order = order;
        buddy_free((void *)addr, order);

        nr_total_free += 1U << order;
        addr += PG_SIZE << order;
    }

    kprintf("kalloc_init: buddy allocator, orders 0-%d, %d free pages (%d KB)\n", MAX_ORDER,
            (int)nr_total_free, (int)(nr_total_free * (PG_SIZE / 1024)));
    buddy_dump();
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
    return buddy_alloc(order);
}

/* Free a multi-page block of 2^order pages. */
void
kfree_pages(void *pa, uint32 order) {
    buddy_free(pa, order);
}
