/*
 * vm.c — Virtual memory management for bobchouOS.
 *
 * Page table creation, mapping, and walk functions.
 * Builds and installs the kernel page table (identity-mapped).
 *
 * See Lecture 4-1 for the full design discussion.
 */

#include "types.h"
#include "riscv.h"
#include "mem_layout.h"
#include "string.h"
#include "kprintf.h"
#include "kalloc.h"
#include "vm.h"

/* Linker-provided symbols (see linker.ld). */
extern char _kernel_start[];
extern char _text_end[];

/* The kernel's own page table, used for all S-mode execution. */
static pte_t *kernel_pagetable;

/*
 * walk — Find or create the leaf PTE for a virtual address.
 *
 * Descends the 3-level page table (levels 2 → 1 → 0).
 * At each non-leaf level:
 *   - If the PTE is valid, follow the pointer (pte_to_pa).
 *   - If invalid and alloc=1, allocate a new table page (kalloc),
 *     zero it, and install a pointer PTE (pa_to_pte | PTE_V).
 *   - If invalid and alloc=0, return NULL.
 * Returns a pointer to the level-0 PTE (so the caller can read
 * or write it), or NULL on failure.
 *
 * TODO: implement the 3-level walk.
 */
pte_t *
walk(pte_t *pagetable, uint64 va, int alloc)
{
    /* TODO */
    (void)pagetable;
    (void)va;
    (void)alloc;
    return NULL;
}

/*
 * map_pages — Install page table mappings for a VA→PA range.
 *
 * For each page in [va, va+size):
 *   1. Call walk(pagetable, a, alloc=1) to get the leaf PTE.
 *   2. Panic if the PTE already has PTE_V set (double-map guard).
 *   3. Write pa_to_pte(pa) | perm | PTE_V | PTE_A | PTE_D.
 *   4. Advance both va and pa by PG_SIZE.
 *
 * Pre-sets PTE_A and PTE_D to avoid page faults on hardware that
 * doesn't set them automatically.
 *
 * Returns 0 on success, -1 if walk() failed to allocate.
 *
 * TODO: implement the per-page mapping loop.
 */
int
map_pages(pte_t *pagetable, uint64 va, uint64 size,
          uint64 pa, int perm)
{
    /* TODO */
    (void)pagetable;
    (void)va;
    (void)size;
    (void)pa;
    (void)perm;
    return -1;
}

/*
 * kvm_map — Map a range into the kernel page table.
 *
 * Thin wrapper: calls map_pages on kernel_pagetable, panics on failure.
 */
static void
kvm_map(uint64 va, uint64 pa, uint64 size, int perm)
{
    if (map_pages(kernel_pagetable, va, size, pa, perm) != 0)
        panic("kvm_map");
}

/*
 * vm_init — Build the kernel page table.
 *
 * Called once from kmain() after kalloc_init().
 * Must:
 *   1. Allocate and zero a root page table page (kalloc).
 *   2. Identity-map PLIC:  PLIC_BASE, PLIC_SIZE, RW
 *   3. Identity-map UART:  UART0_BASE, PG_SIZE, RW
 *   4. Identity-map kernel text: _kernel_start to _text_end, R-X
 *   5. Identity-map kernel data through DRAM end: _text_end to PHYS_STOP, RW
 *   6. Print diagnostic.
 *
 * TODO: allocate root page and call kvm_map for each region.
 */
void
vm_init(void)
{
    /* TODO */
}

/*
 * vm_init_hart — Install the kernel page table and enable paging.
 *
 * Called once from kmain() after vm_init().
 * Must:
 *   1. sfence_vma() — ensure page table writes are visible to hardware walker
 *   2. csrw(satp, MAKE_SATP(kernel_pagetable)) — enable Sv39
 *   3. sfence_vma() — flush stale TLB entries from Bare mode
 *   4. Print diagnostic.
 *
 * After this function returns, every memory access goes through the
 * page table. The identity mapping ensures the kernel keeps working.
 *
 * TODO: enable paging.
 */
void
vm_init_hart(void)
{
    /* TODO */
}
