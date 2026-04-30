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

static void free_range(uint64 pa_start, uint64 pa_end);

/* Convert a physical address to its struct page entry. */
struct page *
pa_to_page(uint64 pa) {
    return &pages[(pa - KERN_BASE) >> PG_SHIFT];
}

/* Initialize the page allocator: set up struct page array, build free list. */
void
kalloc_init(void) {
    nr_pages = (PHYS_STOP - KERN_BASE) >> PG_SHIFT;

    pages = (struct page *)PG_ROUND_UP((uint64)_kernel_end);
    memset(pages, 0, nr_pages * sizeof(struct page));

    free_start = PG_ROUND_UP((uint64)pages + nr_pages * sizeof(struct page));
    free_range(free_start, PHYS_STOP);

    kprintf("kalloc_init: %d free pages (%d KB), page array = %d KB\n", (int)nr_free,
            (int)(nr_free * (PG_SIZE / 1024)), (int)(nr_pages * sizeof(struct page) / 1024));
}

/*
 * Add every page in [pa_start, pa_end) to the free list.
 * Sets refcount to 1 before each kfree() (kfree expects refcount == 1).
 */
static void
free_range(uint64 pa_start, uint64 pa_end) {
    for (uint64 pa = PG_ROUND_UP(pa_start); pa + PG_SIZE <= pa_end; pa += PG_SIZE) {
        pa_to_page(pa)->refcount = 1;
        kfree((void *)pa);
    }
}

/* Free a page: validate, check refcount, junk-fill, prepend to free list. */
void
kfree(void *pa) {
    if ((uint64)pa % PG_SIZE || (uint64)pa < free_start || (uint64)pa >= PHYS_STOP)
        panic("kfree: invalid physical address=%p", pa);

    struct page *pg = pa_to_page((uint64)pa);
    if (pg->refcount != 1)
        panic("kfree: pa=%p refcount=%d", pa, pg->refcount);
    pg->refcount = 0;

    memset(pa, 1, PG_SIZE);

    struct run *r = (struct run *)pa;
    r->next = free_list;
    free_list = r;
    nr_free++;
}

/* Allocate one zeroed 4 KB page. Returns NULL if out of memory. */
void *
kalloc(void) {
    if (!free_list)
        return NULL;

    struct run *r = free_list;
    free_list = r->next;

    pa_to_page((uint64)r)->refcount = 1;
    memset(r, 0, PG_SIZE);
    nr_free--;
    return r;
}
