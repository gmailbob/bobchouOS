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
#define PTE_V (1UL << 0) /* Valid */
#define PTE_R (1UL << 1) /* Readable */
#define PTE_W (1UL << 2) /* Writable */
#define PTE_X (1UL << 3) /* Executable */
#define PTE_U (1UL << 4) /* User-accessible */
#define PTE_G (1UL << 5) /* Global mapping */
#define PTE_A (1UL << 6) /* Accessed */
#define PTE_D (1UL << 7) /* Dirty */

/*
 * PA <-> PTE conversion helpers.
 *
 * A PTE stores the 44-bit PPN in bits 53:10.
 * A physical address stores the PPN in bits 55:12 (bits 11:0 are the
 * page offset, always zero for page-aligned addresses).
 * The 2-bit gap between them (PTE bits 9:8) is the RSW field.
 *
 *   Physical address:  [PPN (55:12)][offset (11:0)]
 *   PTE:               [reserved (63:54)][PPN (53:10)][flags (9:0)]
 *
 * These are inline functions (not macros) for type safety and to avoid
 * double-evaluation — same pattern as csrr/csrw.
 */

/* pa_to_pte: place a physical address's PPN into PTE bit positions.
 *   >> 12: strip the 12-bit page offset, leaving the raw PPN.
 *   << 10: shift PPN up past the 10 flag bits into PTE bits 53:10. */
static inline pte_t
pa_to_pte(uint64 pa) {
    return (pa >> 12) << 10;
}

/* pte_to_pa: extract a physical address from a PTE.
 *   >> 10: shift PPN down past the 10 flag bits, giving the raw PPN.
 *   << 12: shift PPN back up to physical address position (bits 55:12). */
static inline uint64
pte_to_pa(pte_t pte) {
    return (pte >> 10) << 12;
}

/* pte_flags: extract the 10 flag bits (V,R,W,X,U,G,A,D,RSW).
 *   0x3FF = bits 9:0. Masks out the PPN, returns flags only. */
static inline uint64
pte_flags(pte_t pte) {
    return pte & 0x3FF;
}

/*
 * PX — Extract the 9-bit VPN index for a given page table level.
 *
 * Level 0: bits 20:12   (leaf)
 * Level 1: bits 29:21   (middle)
 * Level 2: bits 38:30   (root)
 */
#define PX(level, va) (((uint64)(va) >> (PG_SHIFT + 9 * (level))) & 0x1FF)

/* Maximum virtual address under Sv39 (2^39). */
#define MAX_VA (1UL << 39)

/* ---- Function declarations ---- */

void vm_create_kernel_pt(void);
void vm_enable_paging(void);
pte_t *walk(pte_t *root_pt, uint64 va, int alloc);
int map_pages(pte_t *root_pt, uint64 va, uint64 size, uint64 pa, int perm);

#endif /* VM_H */
