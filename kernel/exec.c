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
#include "elf.h"
#include "kalloc.h"
#include "kmalloc.h"
#include "string.h"
#include "mem_layout.h"
#include "errno.h"

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
    /* TODO(student):
     * 0. If this_proc() has no trapframe (kernel thread calling exec
     *    for the first time, e.g. init_start), allocate one.
     *    Also set context.ra = user_proc_start so swtch returns to user mode.
     * 1. lookup_binary(path) — fail with -ENOENT if not found
     * 2. Validate ELF header (magic, class, machine)
     * 3. Build new page table via proc_pagetable(this_proc())
     * 4. For each PT_LOAD phdr:
     *      - allocate pages, copy filesz bytes, zero remaining
     *      - map into new page table
     *      - create VMA
     * 5. Allocate + map user stack page at USER_STACK_TOP - PG_SIZE
     *    Create stack VMA
     * 6. Push argv onto stack (strings, then pointer array)
     * 7. Free old VMA list + old page table (if any), install new
     * 8. Set trapframe->epc = entry, trapframe->sp = new sp
     * 9. Return argc (or 0 if argv is NULL)
     */
    (void)path;
    (void)argv;
    (void)lookup_binary;
    return -1;
}
