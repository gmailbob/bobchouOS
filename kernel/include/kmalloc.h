/*
 * kmalloc.h — Kernel slab allocator interface.
 *
 * Sub-page allocations via size-class slab pools (32..2048 bytes).
 * Larger requests route to the buddy allocator.
 *
 * See Lecture 4-2 for the full design discussion.
 */

#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h"

void kmalloc_init(void);
void *kmalloc(uint64 size);
void kmfree(void *ptr);

#endif /* KMALLOC_H */
