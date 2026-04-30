/*
 * vm.h — Virtual memory definitions for bobchouOS.
 *
 * PTE type, flag constants, address conversion helpers,
 * and function declarations for page table management.
 *
 * See Lecture 4-1 for the full design discussion.
 */

#ifndef VM_H
#define VM_H

#include "types.h"
#include "riscv.h"

/* Page table entry — a 64-bit value holding PPN + flags. */
typedef uint64 pte_t;

/* ---- PTE flag bits (bits 9:0) ---- */
#define PTE_V   (1UL << 0)   /* Valid */
#define PTE_R   (1UL << 1)   /* Readable */
#define PTE_W   (1UL << 2)   /* Writable */
#define PTE_X   (1UL << 3)   /* Executable */
#define PTE_U   (1UL << 4)   /* User-accessible */
#define PTE_G   (1UL << 5)   /* Global mapping */
#define PTE_A   (1UL << 6)   /* Accessed */
#define PTE_D   (1UL << 7)   /* Dirty */

/*
 * PA <-> PTE conversion helpers.
 *
 * PTE bits 53:10 hold the 44-bit PPN. A physical address has the PPN
 * in bits 55:12 (the low 12 bits are the page offset, always zero for
 * page-aligned addresses).
 *
 * pa_to_pte: shift right 12 (remove offset), left 10 (position into PTE).
 * pte_to_pa: shift right 10, left 12 (reverse).
 * The 2-bit gap (bits 9:8) is the RSW field.
 *
 * These are inline functions (not macros) for type safety and to avoid
 * double-evaluation — same pattern as csrr/csrw.
 */
static inline pte_t
pa_to_pte(uint64 pa)
{
    return (pa >> 12) << 10;
}

static inline uint64
pte_to_pa(pte_t pte)
{
    return (pte >> 10) << 12;
}

static inline uint64
pte_flags(pte_t pte)
{
    return pte & 0x3FF;
}

/*
 * PX — Extract the 9-bit VPN index for a given page table level.
 *
 * Level 0: bits 20:12   (leaf)
 * Level 1: bits 29:21   (middle)
 * Level 2: bits 38:30   (root)
 */
#define PX(level, va)   (((uint64)(va) >> (PG_SHIFT + 9 * (level))) & 0x1FF)

/* Maximum virtual address under Sv39 (2^39). */
#define MAX_VA  (1UL << 39)

/* ---- Function declarations ---- */

void   vm_init(void);
void   vm_init_hart(void);
pte_t *walk(pte_t *pagetable, uint64 va, int alloc);
int    map_pages(pte_t *pagetable, uint64 va, uint64 size,
                 uint64 pa, int perm);

#endif /* VM_H */
