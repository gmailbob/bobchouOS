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
#include "proc.h"

/* Linker-provided symbols (see linker.ld). */
extern char _kernel_start[];
extern char _text_end[];

/* Trampoline entry points (defined in trampoline.S, page-aligned by linker). */
extern char user_vec[];
extern char user_ret[];

/* The kernel's own page table, used for all S-mode execution. */
static pte_t *kernel_root_pt;

/*
 * walk — Find or create the leaf PTE for a virtual address.
 *
 * Descends the 3-level page table (levels 2 → 1 → 0).
 * alloc: if 1, allocate missing intermediate pages; if 0, return NULL.
 * At each non-leaf level:
 *   - If the PTE is valid, follow the pointer (pte_to_pa).
 *   - If invalid and alloc=1, allocate a new table page (kalloc),
 *     zero it, and install a pointer PTE (pa_to_pte | PTE_V).
 *   - If invalid and alloc=0, or if kalloc fails, return NULL.
 * Returns a pointer to the level-0 PTE (so the caller can read
 * or write it), or NULL on failure. The caller decides whether to
 * panic (boot-time) or handle gracefully (runtime).
 */
pte_t *
walk(pte_t *root_pt, uint64 va, int alloc) {
    if (va >= MAX_VA)
        panic("walk: va >= MAX_VA");
    pte_t *curr_table = root_pt, *p;
    for (int level = 2; level > 0; level--) {
        p = &curr_table[PX(level, va)];
        if (*p & PTE_V)
            curr_table = (pte_t *)pte_to_pa(*p);
        else if (alloc) {
            if (!(curr_table = kalloc())) /* kalloc returns zeroed — all PTEs start V=0 */
                return NULL;
            *p = pa_to_pte((uint64)curr_table) | PTE_V;
        } else
            return NULL;
    }
    return &curr_table[PX(0, va)];
}

/*
 * map_pages — Install page table mappings for a VA→PA range.
 *
 * perm: permission flags (OR of PTE_R, PTE_W, PTE_X, etc.).
 * For each page in [va, va+size):
 *   1. Call walk(root_pt, a, alloc=1) to get the leaf PTE.
 *   2. Panic if the PTE already has PTE_V set (double-map guard).
 *   3. Write pa_to_pte(pa) | perm | PTE_V | PTE_A | PTE_D.
 *   4. Advance both va and pa by PG_SIZE.
 *
 * Pre-sets PTE_A and PTE_D to avoid page faults on hardware that
 * doesn't set them automatically.
 *
 * Returns 0 on success, -1 if walk() failed to allocate.
 */
int
map_pages(pte_t *root_pt, uint64 va, uint64 size, uint64 pa, int perm) {
    if (size == 0)
        panic("map_pages: size == 0");
    if (va % PG_SIZE || pa % PG_SIZE || size % PG_SIZE)
        panic("map_pages: not page-aligned");
    int nr_page = size >> PG_SHIFT;
    while (nr_page--) {
        pte_t *leaf = walk(root_pt, va, 1);
        if (!leaf)
            return -1;
        if (*leaf & PTE_V)
            panic("map_pages: double-map va=%p", (void *)va);
        *leaf = pa_to_pte(pa) | perm | PTE_V | PTE_A | PTE_D;
        va += PG_SIZE;
        pa += PG_SIZE;
    }
    return 0;
}

/*
 * kvm_map — Map a range into the kernel page table.
 *
 * Thin wrapper: calls map_pages on kernel_root_pt, panics on failure.
 */
static void
kvm_map(uint64 va, uint64 pa, uint64 size, int perm) {
    if (map_pages(kernel_root_pt, va, size, pa, perm))
        panic("kvm_map");
}

/*
 * vm_create_kernel_pt — Build the kernel page table.
 *
 * Called once from kmain() after kalloc_init().
 * Must:
 *   1. Allocate and zero a root page table page (kalloc).
 *   2. Identity-map PLIC:  PLIC_BASE, PLIC_SIZE, RW
 *   3. Identity-map UART:  UART0_BASE, PG_SIZE, RW
 *   4. Identity-map kernel text: _kernel_start to _text_end, R-X
 *   5. Identity-map kernel data through DRAM end: _text_end to PHYS_STOP, RW
 *   6. Print diagnostic.
 */
void
vm_create_kernel_pt(void) {
    if (!(kernel_root_pt = kalloc()))
        panic("vm_init: failed to kalloc");
    kvm_map(QEMU_SHUTDOWN, QEMU_SHUTDOWN, PG_SIZE, PTE_R | PTE_W);
    kvm_map(CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);
    kvm_map(PLIC_BASE, PLIC_BASE, PLIC_SIZE, PTE_R | PTE_W);
    kvm_map(UART0_BASE, UART0_BASE, PG_SIZE, PTE_R | PTE_W);
    kvm_map((uint64)_kernel_start, (uint64)_kernel_start, (uint64)_text_end - (uint64)_kernel_start,
            PTE_R | PTE_X);
    kvm_map((uint64)_text_end, (uint64)_text_end, PHYS_STOP - (uint64)_text_end, PTE_R | PTE_W);
    /* Trampoline: the ONLY non-identity mapping in the kernel PT. */
    kvm_map(TRAMPOLINE, (uint64)user_vec, PG_SIZE, PTE_R | PTE_X);
    kprintf("vm_create_kernel_pt: page table at %p\n", kernel_root_pt);
}

/*
 * vm_enable_paging — Write satp to activate the kernel page table.
 *
 * Called once from kmain() after vm_create_kernel_pt().
 * Must:
 *   1. sfence_vma() — ensure page table writes are visible to hardware walker
 *   2. csrw(satp, MAKE_SATP(kernel_root_pt)) — enable Sv39
 *   3. sfence_vma() — flush stale TLB entries from Bare mode
 *   4. Print diagnostic.
 *
 * After this function returns, every memory access goes through the
 * page table. The identity mapping ensures the kernel keeps working.
 */
void
vm_enable_paging(void) {
    sfence_vma();
    csrw(satp, MAKE_SATP(kernel_root_pt));
    sfence_vma();
    kprintf("vm_enable_paging: Sv39 enabled\n");
}

/*
 * proc_pagetable — Create a per-process user page table.
 *
 * Allocates a root page table page and maps:
 * 1. Trampoline at TRAMPOLINE VA (R-X, no PTE_U)
 * 2. This process's trapframe at TRAPFRAME VA (R-W, no PTE_U)
 *
 * User text and stack are mapped by the caller.
 * Returns the root page table, or NULL on failure.
 *
 * See Lecture 6-1, Part 5.
 */
pte_t *
proc_pagetable(struct proc *p) {
    pte_t *user_root_pt = (pte_t *)kalloc();
    if (!user_root_pt)
        return NULL;
    int fail = 0;
    fail |= map_pages(user_root_pt, TRAMPOLINE, PG_SIZE, (uint64)user_vec, PTE_R | PTE_X);
    fail |= map_pages(user_root_pt, TRAPFRAME, PG_SIZE, (uint64)p->trapframe, PTE_R | PTE_W);
    if (fail) {
        proc_free_pagetable(user_root_pt, PG_SIZE);
        return NULL;
    }
    return user_root_pt;
}

/*
 * proc_free_pagetable — Free a user page table (intermediate pages only).
 *
 * Walks the 3-level tree and frees intermediate page table pages.
 * User data pages (text, stack, trapframe) are freed separately.
 *
 * See Lecture 6-1, Part 9.
 */
void
proc_free_pagetable(pte_t *root, uint64 sz) {
    /* Walk the 3-level tree, freeing level-0 and level-1 table pages.
     * Does NOT free leaf pages (user text, stack) — those are freed by caller. */
    for (int i = 0; i < PG_SIZE / sizeof(pte_t); i++) {
        if (!(root[i] & PTE_V))
            continue;
        pte_t *lv1 = (pte_t *)pte_to_pa(root[i]);
        for (int j = 0; j < PG_SIZE / sizeof(pte_t); j++) {
            if (lv1[j] & PTE_V)
                kfree((void *)pte_to_pa(lv1[j])); /* free level-0 table page */
        }
        kfree(lv1); /* free level-1 table page */
    }
    kfree(root); /* free root (level-2) page */
    (void)sz;
}

/* --- User memory access (Round 6-2) --- */

/*
 * copyin — Copy bytes from user virtual address to kernel buffer.
 *
 * Walks the user page table to translate each page of srcva, then copies
 * via the kernel identity map. Validates PTE_V and PTE_U on each page.
 *
 * Returns 0 on success, -EFAULT on invalid user address.
 * See Lecture 6-2, Part 5.
 */
int
copyin(pte_t *pagetable, void *dst, uint64 srcva, uint64 len) {
    /* TODO: page-at-a-time loop:
     *   - PGROUNDDOWN(srcva) to get page base
     *   - walk(pagetable, va_page, 0) to get PTE
     *   - validate PTE_V and PTE_U (return -EFAULT if bad)
     *   - compute PA = pte_to_pa(*pte) + offset within page
     *   - compute chunk size (min of remaining len, bytes left in page)
     *   - memcpy(dst, (void *)pa, chunk)
     *   - advance srcva, dst, decrement len
     */
    (void)pagetable; (void)dst; (void)srcva; (void)len;
    return -1;
}

/*
 * copyout — Copy bytes from kernel buffer to user virtual address.
 *
 * Same as copyin but in reverse. Also checks PTE_W (must not write to
 * a read-only user page).
 *
 * Returns 0 on success, -EFAULT on invalid user address.
 * See Lecture 6-2, Part 5.
 */
int
copyout(pte_t *pagetable, uint64 dstva, void *src, uint64 len) {
    /* TODO: same loop as copyin, but:
     *   - additionally check PTE_W
     *   - memcpy((void *)pa, src, chunk) (reversed direction)
     */
    (void)pagetable; (void)dstva; (void)src; (void)len;
    return -1;
}
