/*
 * exec.c — program loading: turn an ELF image into a running address space.
 *
 * proc_exec("name", argv) replaces the calling process's entire user
 * address space with a fresh one built from a named ELF binary, then
 * arranges for the process to resume at the new program's entry point.
 * The PID, kernel stack, parent/child links, and open resources survive;
 * only the user memory is replaced.
 *
 * Since there is no filesystem yet (Phase 7), binaries are compiled
 * separately, linked as ELF, and embedded in the kernel image via .incbin
 * (see kernel/arch/user_bin_*.S). lookup_binary() resolves a name to the
 * embedded bytes; when a real filesystem arrives, only lookup_binary
 * changes — proc_exec stays the same.
 *
 * Build-new-then-swap discipline: proc_exec constructs the new page table,
 * VMAs, and stack in locals and only commits them to the process after the
 * last step that can fail. Any earlier failure frees the partial new state
 * and returns -1, leaving the old address space untouched (exec is a no-op
 * on failure, per POSIX).
 *
 * NOTE: the embedded ELF is trusted (we built it). Program-header fields
 * (e_phoff, p_offset, p_filesz, p_vaddr, ...) are not bounds-checked here.
 * When Phase 7 loads untrusted binaries from disk, those checks must be
 * added (see Lecture 6-3 review notes).
 *
 * See Lecture 6-3, Parts 4 and 5.
 */

#include "types.h"
#include "proc.h"
#include "vma.h"
#include "vm.h"
#include "elf_defs.h"
#include "kalloc.h"
#include "kmalloc.h"
#include "string.h"
#include "mem_layout.h"
#include "riscv.h"
#include "errno.h"
#include "kprintf.h"
#include "trapframe.h"

extern void user_trap(void);

/* --- Embedded binary table --- */

extern char _binary_init_start[], _binary_init_end[];
extern char _binary_hello_start[], _binary_hello_end[];

struct embedded_binary {
    const char *name;
    const uint8 *start;
    const uint8 *end;
};

static struct embedded_binary binaries[] = {
    {"init", (uint8 *)_binary_init_start, (uint8 *)_binary_init_end},
    {"hello", (uint8 *)_binary_hello_start, (uint8 *)_binary_hello_end},
};

#define NBINARIES (sizeof(binaries) / sizeof(binaries[0]))

/* Max argv entries proc_exec will place on the new stack (excluding the
 * NULL terminator). Bounds the on-stack uargv[] scratch array. */
#define MAXARG 16

static int
elf_to_pte(uint32 flags) {
    int perm = PTE_U;
    if (flags & PF_R)
        perm |= PTE_R;
    if (flags & PF_W)
        perm |= PTE_W;
    if (flags & PF_X)
        perm |= PTE_X;
    return perm;
}

/*
 * lookup_binary — find an embedded binary by name.
 *
 * Returns pointer to the entry, or NULL if not found.
 */
static const struct embedded_binary *
lookup_binary(const char *name) {
    for (uint64 i = 0; i < NBINARIES; i++)
        if (strcmp(binaries[i].name, name) == 0)
            return &binaries[i];
    return 0;
}

/*
 * proc_exec — replace the current process's program with a new one.
 *
 * Steps:
 *   1. Ensure a trapframe exists (a first-time kernel thread, e.g. init,
 *      has none yet; a user process already does).
 *   2. Look up and validate the named ELF binary.
 *   3. Build a NEW page table and VMA list in locals (build-new-then-swap).
 *   4. Map each PT_LOAD segment: alloc zeroed pages, copy filesz bytes
 *      (the rest stays zero = .bss).
 *   5. Allocate and map a one-page user stack; push argv onto it.
 *   6. Commit: free the OLD address space, install the new one, and set
 *      the trapframe so the process resumes at the ELF entry point with
 *      a0=argc, a1=argv.
 *
 * Returns argc on success — but does NOT transfer to user mode itself.
 *   - sys_exec path: returns into the syscall handler, which returns into
 *     user_trap_ret, which enters user mode at the new epc.
 *   - init_start path: a kernel thread; it calls user_trap_ret() itself
 *     after proc_exec returns.
 * Returns -1 on failure, leaving the old address space fully intact.
 *
 * (On success a0 ends up = argc both because the trapframe is set here and
 * because the syscall dispatcher writes proc_exec's return value to a0 —
 * the two agree by design.)
 */
int
proc_exec(const char *path, char **argv) {
    struct proc *p = this_proc();

    /* Allocate trapframe if this is a kernel thread calling exec for the
     * first time (e.g. init_start). User processes already have one. */
    if (!p->trapframe) {
        p->trapframe = kalloc();
        if (!p->trapframe)
            return -ENOMEM;
        memset(p->trapframe, 0, PG_SIZE);
    }

    const struct embedded_binary *bin = lookup_binary(path);
    if (!bin) {
        kprintf("exec: binary '%s' not found\n", path);
        return -ENOENT;
    }

    struct elf_header *elf = (struct elf_header *)bin->start;
    uint8 *e_ident = elf->e_ident;
    if (*(uint32 *)e_ident != ELF_MAGIC || e_ident[EI_CLASS] != ELFCLASS64 ||
        e_ident[EI_DATA] != ELFDATA2LSB || elf->e_machine != EM_RISCV) {
        kprintf("exec: bad ELF header for '%s'\n", path);
        return -ENOENT;
    }

    /* Build new page table + VMA list. Keep old alive until success
     * (so failure leaves the process unchanged). */
    pte_t *new_pt = proc_pagetable(p);
    if (!new_pt)
        return -ENOMEM;
    LIST_HEAD(new_vma_list);

    /* Load PT_LOAD segments */
    struct elf_phdr *phdrs = (struct elf_phdr *)((uint8 *)elf + elf->e_phoff);
    for (uint16 i = 0; i < elf->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;

        uint8 *src = (uint8 *)elf + phdrs[i].p_offset;
        uint64 start = PG_ROUND_DOWN(phdrs[i].p_vaddr);
        uint64 end = PG_ROUND_UP(phdrs[i].p_vaddr + phdrs[i].p_memsz);
        int perm = elf_to_pte(phdrs[i].p_flags);
        uint64 filesz_remaining = phdrs[i].p_filesz;

        for (uint64 va = start; va < end; va += PG_SIZE) {
            void *pg = kalloc(); /* kalloc returns zeroed page */
            if (!pg)
                goto fail;
            /* Copy file data (may be less than a full page) */
            uint64 chunk = filesz_remaining > PG_SIZE ? PG_SIZE : filesz_remaining;
            if (chunk > 0) {
                memcpy(pg, src, chunk);
                src += chunk;
                filesz_remaining -= chunk;
            }
            /* Remaining bytes are already zero (bss) from kalloc */
            if (map_pages(new_pt, va, PG_SIZE, (uint64)pg, perm) < 0) {
                kfree(pg);
                goto fail;
            }
        }

        struct vma *v = vma_create(start, end, perm);
        if (!v)
            goto fail;
        list_add_tail(&v->link, &new_vma_list);
    }

    /* Allocate user stack */
    uint8 *stack = kalloc();
    if (!stack)
        goto fail;
    uint64 ustack_bot = USER_STACK_TOP - PG_SIZE;
    if (map_pages(new_pt, ustack_bot, PG_SIZE, (uint64)stack, PTE_R | PTE_W | PTE_U) < 0) {
        kfree(stack);
        goto fail;
    }
    struct vma *stack_vma = vma_create(ustack_bot, USER_STACK_TOP, PTE_R | PTE_W | PTE_U);
    if (!stack_vma)
        goto fail;
    list_add_tail(&stack_vma->link, &new_vma_list);

    /* Push argv onto the new stack, growing down from the top.
     *
     * Layout (high -> low address):
     *   [arg strings][padding][argv[] pointer array + NULL]
     *                                                       ^ sp (16-aligned)
     * a0=argc, a1=&argv[0] are set in the trapframe below so _start sees them.
     *
     * The stack is one page. Every decrement of sp is bounds-checked against
     * ustack_bot: an oversized argv must fail cleanly (-E2BIG), never write
     * past the page (which, with unsigned sp - ustack_bot, would wrap to a
     * wild kernel address). uargv[] is capped at MAXARG. */
    uint64 sp = USER_STACK_TOP;
    uint64 uargv[MAXARG];
    int argc = 0;

    for (; argv && argv[argc]; argc++) {
        if (argc >= MAXARG)
            goto fail;
        uint64 len = strlen(argv[argc]) + 1; /* include the '\0' */
        sp -= len;
        if (sp < ustack_bot) /* string would overflow the stack page */
            goto fail;
        memcpy(stack + (sp - ustack_bot), argv[argc], len);
        uargv[argc] = sp; /* remember the user VA where this string landed */
    }

    sp &= ~15UL; /* 16-byte align before pushing the pointer array */

    /* Reserve argc pointers + a NULL terminator. */
    sp -= (uint64)(argc + 1) * 8;
    if (sp < ustack_bot)
        goto fail;
    for (int i = 0; i < argc; i++)
        *(uint64 *)(stack + (sp + i * 8 - ustack_bot)) = uargv[i];
    *(uint64 *)(stack + (sp + (uint64)argc * 8 - ustack_bot)) = 0; /* argv[argc] = NULL */

    /* === Success: swap old address space for new === */
    vma_free_all(p); /* free old user pages */
    if (p->pagetable)
        proc_free_pagetable(p->pagetable); /* free old page table */
    p->pagetable = new_pt;
    /* Move new VMAs into process's list */
    INIT_LIST_HEAD(&p->vma_list);
    if (!list_empty(&new_vma_list)) {
        p->vma_list.next = new_vma_list.next;
        p->vma_list.prev = new_vma_list.prev;
        new_vma_list.next->prev = &p->vma_list;
        new_vma_list.prev->next = &p->vma_list;
    }

    /* Set up trapframe for user entry */
    p->trapframe->epc = elf->e_entry;
    p->trapframe->sp = sp;
    p->trapframe->a0 = argc; /* main's first arg */
    p->trapframe->a1 = sp;   /* main's second arg: &argv[0] on the stack */
    p->trapframe->kernel_satp = csrr(satp);
    p->trapframe->kernel_sp = p->kstack + PG_SIZE;
    p->trapframe->user_trap = (uint64)user_trap;
    p->trapframe->hartid = 0;

    /* Update process name to the new program (like Linux's comm field) */
    memset(p->name, 0, sizeof(p->name));
    for (int i = 0; i < PROC_NAME_LEN - 1 && path[i]; i++)
        p->name[i] = path[i];

    return argc;

fail:
    /* Clean up partially-built new address space */
    struct vma *pos, *tmp;
    list_for_each_entry_safe(pos, tmp, &new_vma_list, link) {
        for (uint64 va = pos->start; va < pos->end; va += PG_SIZE) {
            pte_t *pte = walk(new_pt, va, 0);
            if (!pte || !(*pte & PTE_V))
                continue;
            page_put((void *)pte_to_pa(*pte));
        }
        list_del(&pos->link);
        kmfree(pos);
    }
    proc_free_pagetable(new_pt);
    return -1;
}
