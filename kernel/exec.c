/*
 * exec.c — ELF loading and binary lookup.
 *
 * Provides:
 *   lookup_binary(name) — find embedded ELF by name
 *   proc_exec(path, argv) — replace current process's address space
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
#include "errno.h"
#include "trapframe.h"

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
 * proc_exec — replace current process with a new program.
 *
 * Loads an ELF binary, builds a new page table + VMA list,
 * sets up the user stack with argv, then swaps the old address space.
 *
 * Returns argc on success (never returns to old code).
 * Returns -1 on failure (old process continues unchanged).
 *
 * Called from:
 *   - init_start (PID 1 kernel thread execs into user init)
 *   - sys_exec (user process replaces itself)
 */
int
proc_exec(const char *path, char **argv) {
    struct proc *p = this_proc();
    if (!p->trapframe) {
        if (!(p->trapframe = kalloc()))
            return -ENOMEM;
        // p->context.ra = (uint64)user_proc_start;
    }

    const struct embedded_binary *bin = lookup_binary(path);
    if (!bin)
        return -ENOENT;

    struct elf_header *elf = (struct elf_header *)bin->start;
    uint8 *e_ident = elf->e_ident;
    if (*(uint32 *)e_ident != ELF_MAGIC || e_ident[EI_CLASS] != ELFCLASS64 ||
        e_ident[EI_DATA] != ELFDATA2LSB || elf->e_machine != EM_RISCV)
        return -ENOENT;

    proc_free_pagetable(p->pagetable);
    p->pagetable = proc_pagetable(p);
    vma_free_all(p);

    struct elf_phdr *phdrs = (struct elf_phdr *)((uint64)bin->start + sizeof(struct elf_header));
    for (uint16 i = 0; i < elf->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;

        uint8 *src = (uint8 *)elf + phdrs[i].p_offset;

        uint64 start = PG_ROUND_DOWN(phdrs[i].p_vaddr);
        uint64 end = PG_ROUND_UP(phdrs[i].p_vaddr + phdrs[i].p_memsz);
        int perm = elf_to_pte(phdrs[i].p_flags);
        for (uint64 va = start; va < end; va += PG_SIZE) {
            void *pg = kalloc();
            memcpy(pg, src, PG_SIZE);
            src += PG_SIZE;

            map_pages(p->pagetable, va, PG_SIZE, (uint64)pg, perm);
        }

        vma_add(p, vma_create(start, end, perm));
    }

    uint8 *stack = kalloc();
    uint64 ustack_bot = USER_STACK_TOP - PG_SIZE;
    map_pages(p->pagetable, ustack_bot, PG_SIZE, (uint64)stack, PTE_R | PTE_W | PTE_U);

    uint64 sp = USER_STACK_TOP;

    uint64 uargv[16];
    int argc = 0;
    for (int i = 0; argv && argv[i]; i++) {
        int len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy(stack + (sp - ustack_bot), argv[i], len);
        uargv[i] = sp;
        argc++;
    }

    sp &= ~15UL;

    sp -= (argc + 1) * 8;
    for (int i = 0; i < argc; i++) {
        *(uint64 *)(stack + (sp + i * 8 - ustack_bot)) = uargv[i];
    }
    *(uint64 *)(stack + (sp + argc * 8 - ustack_bot)) = 0;

    p->trapframe->epc = elf->e_entry;
    p->trapframe->sp = sp;

    return argc;
}
