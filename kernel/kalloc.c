/*
 * kalloc.c — Physical page allocator for bobchouOS.
 *
 * Manages free 4 KB pages using a singly-linked free list threaded
 * through the free pages themselves (struct run). A struct page array
 * provides per-page metadata (refcount) for every physical page.
 *
 * See Lecture 3-2 for the full design discussion.
 */

#include "types.h"
#include "riscv.h"
#include "mem_layout.h"
#include "string.h"
#include "kprintf.h"
#include "kalloc.h"

extern char _kernel_end[];

struct run {
    struct run *next;
};

static struct page *pages;
static uint64 nr_pages;

static struct run *free_list;
static uint64 free_start;
static uint64 nr_free;

static void free_range(void *pa_start, void *pa_end);

/*
 * pa_to_page — Convert a physical address to its struct page entry.
 *
 * TODO: compute the array index from pa and return &pages[index].
 * The index is the page's offset from KERN_BASE, divided by PG_SIZE.
 */
struct page *
pa_to_page(uint64 pa) {
    return pages + ((pa - KERN_BASE) >> PG_SHIFT);
}

/*
 * kinit — Initialize the page allocator.
 *
 * Called once from kmain() at boot. Must:
 *   1. Compute nr_pages (total physical pages from KERN_BASE to PHYS_STOP)
 *   2. Place the struct page array at PG_ROUND_UP(_kernel_end)
 *   3. Zero the array (all refcounts start at 0)
 *   4. Compute free_start: PG_ROUND_UP(end of struct page array)
 *   5. Call free_range(free_start, PHYS_STOP)
 *   6. Print boot diagnostic: free pages, KB, array size
 */
void
kinit(void) {
    nr_pages = (PHYS_STOP - KERN_BASE) >> PG_SHIFT;

    pages = (struct page *)PG_ROUND_UP((uint64)_kernel_end);
    // uint64 i;
    // for (i = 0; i < nr_pages; i++) {
    //     (pages + i)->refcount = 0;
    // }

    free_start = KERN_BASE + sizeof(struct page) * nr_pages;
    free_range((void *)free_start, (void *)PHYS_STOP);

    kprintf("free pages=%d, pages size=%d", (int)nr_free, (int)nr_pages);
}

/*
 * free_range — Add every page in [pa_start, pa_end) to the free list.
 *
 * Round pa_start up to a page boundary, then loop page-by-page,
 * calling kfree() on each. Must set refcount to 1 before each
 * kfree() call (kfree expects refcount == 1).
 */
static void
free_range(void *pa_start, void *pa_end) {
    uint64 pa;
    for (pa = PG_ROUND_UP((uint64)pa_start); pa < PHYS_STOP; pa += PG_SIZE) {
        pa_to_page(pa)->refcount = 1;
        kfree((void *)pa);
    }
}

/*
 * kfree — Free a page of physical memory.
 *
 * Validate the address (page-aligned, in the free region, below PHYS_STOP).
 * Check that refcount == 1 (catches double free and shared-page mistakes).
 * Set refcount to 0, fill the page with junk (0x01), prepend to free list.
 */
void
kfree(void *pa) {
    if ((uint64)pa % PG_SIZE || (uint64)pa < free_start || (uint64)pa >= PHYS_STOP)
        panic("kfree: invalid physical address=%p", pa);

    struct page *p = pa_to_page((uint64)pa);
    if (p->refcount > 1)
        panic("kfree: refcount=%d", p->refcount);
    p->refcount = 0;

    memset(pa, 1, PG_SIZE);

    ((struct run *)pa)->next = free_list;
    free_list = pa;
    nr_free++;
}

/*
 * kalloc — Allocate one 4 KB physical page.
 *
 * Pop the head of the free list. If non-NULL, zero the page
 * and set its refcount to 1. Return the page address, or NULL
 * if out of memory.
 */
void *
kalloc(void) {
    if (!free_list)
        return NULL;

    struct run *p = free_list;
    free_list = free_list->next;

    pa_to_page((uint64)p)->refcount = 1;
    memset(p, 0, PG_SIZE);
    nr_free--;
    return p;
}
