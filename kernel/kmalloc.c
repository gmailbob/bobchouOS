/*
 * kmalloc.c — Slab allocator for bobchouOS.
 *
 * Provides sub-page allocations using 7 power-of-two size classes
 * (32, 64, 128, 256, 512, 1024, 2048 bytes). Each class maintains
 * a linked list of slab pages. Slab metadata lives in struct page
 * (external, not embedded in the slab page).
 *
 * Allocations larger than 2048 bytes route to the buddy allocator.
 *
 * See Lecture 4-2 for the full design discussion.
 */

#include "types.h"
#include "riscv.h"
#include "string.h"
#include "kprintf.h"
#include "kalloc.h"
#include "kmalloc.h"

#define NR_SIZE_CLASSES 7
#define MIN_ALLOC_SIZE 32
#define MAX_SLAB_SIZE 2048

/* Link for the slab free list — overlaid on the first 8 bytes of a free slot. */
struct slot {
    struct slot *next;
};

/* One per size class. */
struct size_class {
    uint32 slot_size;
    struct page *slabs; /* linked list via page->slab.next_slab */
};

static struct size_class size_classes[NR_SIZE_CLASSES];

/*
 * size_to_class — Find the smallest size class that fits `size` bytes.
 *
 * Returns the class index (0-6), or -1 if size exceeds MAX_SLAB_SIZE.
 */
static int
size_to_class(uint64 size) {
    for (int i = 0; i < NR_SIZE_CLASSES; i++)
        if (size <= size_classes[i].slot_size)
            return i;
    return -1;
}

/*
 * slab_create — Allocate and initialize a new slab page for a size class.
 *
 * 1. Call kalloc() to get a fresh page.
 * 2. Set up struct page fields:
 *    - flags = PG_SLAB
 *    - slab.class_idx = the class index
 *    - slab.nr_alloc = 0
 *    - slab.next_slab = NULL
 * 3. Divide the entire page into slots of size_classes[idx].slot_size.
 *    Build a free list by threading struct slot pointers through
 *    each slot. The first slot starts at the beginning of the page
 *    (no embedded header — metadata is in struct page).
 * 4. Set page->slab.free_list to the head of the slot free list.
 * 5. Return the struct page pointer (or NULL if kalloc failed).
 */
static struct page *
slab_create(int class_idx) {
    if (class_idx < 0 || class_idx >= NR_SIZE_CLASSES)
        panic("slab_create: class_idx=%d", class_idx);

    struct slot *s = kalloc();
    if (s == NULL)
        return NULL;

    struct page *pg = pa_to_page((uint64)s);
    pg->flags = PG_SLAB;
    pg->slab.class_idx = class_idx;
    pg->slab.nr_alloc = 0;
    pg->slab.free_list = s;
    pg->slab.next_slab = NULL;

    uint32 slot_size = size_classes[class_idx].slot_size;
    for (int i = 0; i < PG_SIZE / slot_size - 1; i++) {
        struct slot *next = (struct slot *)((uint64)s + slot_size);
        s->next = next;
        s = next;
    }
    s->next = NULL;

    return pg;
}

/*
 * slab_destroy — Return an empty slab page to the buddy allocator.
 *
 * Unlink the slab from its class's slab list, then kfree() the page.
 * The class's slab list is singly-linked (via page->slab.next_slab),
 * so unlinking requires scanning to find the previous entry.
 */
static void
slab_destroy(struct page *slab, int class_idx) {
    if (slab == NULL || class_idx < 0 || class_idx >= NR_SIZE_CLASSES)
        panic("slab_destroy: slab=%p class_idx=%d", slab, class_idx);
    struct page **ps = &size_classes[class_idx].slabs;
    if (*ps == NULL || (*ps)->slab.next_slab == NULL) // keep at least one active slab
        return;

    while (*ps && *ps != slab)
        ps = &(*ps)->slab.next_slab;

    if (*ps) {
        (*ps)->slab.next_slab = slab->slab.next_slab;
        kfree((void *)page_to_pa(slab));
    } else
        panic("slab_destroy: slab=%p not found", slab);
}

/*
 * kmalloc — Allocate size bytes of kernel memory.
 *
 * - If size == 0, return NULL.
 * - If size > MAX_SLAB_SIZE (big-alloc path):
 *     1. Compute the buddy order needed to hold size bytes.
 *     2. Call kalloc_pages(order).
 *     3. Set page->flags = PG_BIG on the first page.
 *     4. Return the pointer (entire block is usable).
 * - Otherwise (slab path):
 *     1. Find the size class via size_to_class().
 *     2. Check the first slab in the class's list. If it has a free
 *        slot (page->slab.free_list != NULL), pop it.
 *     3. If no free slots, call slab_create() to make a new slab,
 *        prepend it to the class's slab list, and pop a slot.
 *     4. Increment page->slab.nr_alloc.
 *     5. Return the slot pointer.
 */
void *
kmalloc(uint64 size) {
    if (size == 0)
        return NULL;

    if (size > MAX_SLAB_SIZE) {
        uint32 order = 0;
        while (order <= MAX_ORDER && (PG_SIZE << order) < size)
            order++;
        if (order > MAX_ORDER)
            panic("kmalloc: size=%d exceeds limit", size);

        void *pa = kalloc_pages(order);
        pa_to_page((uint64)pa)->flags = PG_BIG;
        return pa;
    }

    int idx = size_to_class(size);
    struct page **ps = &size_classes[idx].slabs;
    while (*ps && (*ps)->slab.free_list == NULL)
        ps = &(*ps)->slab.next_slab;

    if (*ps == NULL) {
        if ((*ps = slab_create(idx)) == NULL)
            return NULL;
    }

    (*ps)->slab.nr_alloc++;
    struct slot *s = (*ps)->slab.free_list;
    (*ps)->slab.free_list = s->next;
    return s;
}

/*
 * kmfree — Free memory previously returned by kmalloc.
 *
 * 1. Look up the struct page via pa_to_page((uint64)ptr).
 * 2. If page->flags & PG_BIG:
 *      Clear page->flags, then call kfree_pages(ptr, page->order).
 * 3. Otherwise (slab path):
 *      a. Junk-fill the slot with 0xAB (size = size_classes[class_idx].slot_size).
 *      b. Push the slot onto page->slab.free_list.
 *      c. Decrement page->slab.nr_alloc.
 *      d. If nr_alloc == 0 and the class has another empty slab,
 *         call slab_destroy() to return this page to the buddy allocator.
 *         Clear page->flags before calling kfree().
 *         Otherwise keep it (one empty slab per class is fine).
 */
void
kmfree(void *ptr) {
    struct page *pg = pa_to_page((uint64)ptr);
    if (pg->flags & PG_BIG) {
        pg->flags = 0;
        return kfree_pages(ptr, pg->order);
    }

    uint8 idx = pg->slab.class_idx;
    memset(ptr, 0xAB, size_classes[idx].slot_size);
    struct slot *s = (struct slot *)ptr;
    s->next = pg->slab.free_list;
    pg->slab.free_list = s;

    if (--pg->slab.nr_alloc == 0)
        slab_destroy(pg, idx);
}

/*
 * kmalloc_init — Initialize the slab allocator.
 *
 * 1. Set up size_classes[]: slot_size = 32, 64, 128, 256, 512, 1024, 2048.
 *    Initialize slabs = NULL for each class.
 * 2. Pre-allocate one slab per class via slab_create(). This ensures
 *    every class always has at least one slab, so the allocation fast
 *    path never needs to check "does a slab exist?"
 * 3. Print diagnostic: "kmalloc_init: 7 size classes (32..2048),
 *    big alloc > 2048"
 */
void
kmalloc_init(void) {
    for (uint8 i = 0; i < NR_SIZE_CLASSES; i++) {
        size_classes[i].slot_size = 32 << i;
        size_classes[i].slabs = slab_create(i);
    }
    kprintf("kmalloc_init: 7 size classes (32..2048)");
}
