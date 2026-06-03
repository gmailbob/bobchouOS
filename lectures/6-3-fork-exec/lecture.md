# Lecture 6-3: fork, exec, and Process Syscalls

> **Where we are**
>
> Round 6-2 built the syscall layer: dispatch table, argument
> extraction, copyin/copyout, and two real syscalls (write and exit).
> A user program prints "hello world" and exits cleanly. The full
> user/kernel round-trip works end-to-end.
>
> But user programs can't reproduce. There is no way to create new
> processes from user space, no way to run a different program, and no
> way to wait for a child. The kernel creates every user process
> by hand (proc_create_user_test), hardcoded in C. We have
> a single user process with no offspring. That's not an operating
> system — it's a demo.
>
> This round introduces five new syscalls: `fork()` duplicates a
> running process, `exec()` replaces its code with a new program,
> `wait()` lets a parent collect its child's exit status, `getpid()`
> returns the process's identity, and `kill()` terminates another
> process. Together with `write()` and `exit()` from Round 6-2, they
> form the complete process API.
>
> By the end of this round, a real init process (loaded from an
> embedded ELF binary) will fork, exec a child program, wait for it to
> finish, and loop forever — the birth of a process tree.
>
> **What you will understand after this lecture:**
>
> - The VMA (Virtual Memory Area) abstraction for tracking address spaces
> - How fork duplicates a process's entire virtual memory
> - Why Unix uses fork+exec (two steps) instead of a single spawn
> - The ELF binary format and how exec loads a program from scratch
> - How user programs are embedded in the kernel (without a filesystem)
> - How argc/argv are pushed onto the new stack by exec
> - page_get/page_put — reference-counted physical pages
> - The full init lifecycle: fork, exec, wait, loop

> **xv6 book coverage:**
> This lecture absorbs Ch 3 §3.8 ("Code: exec") fully, plus the fork
> portions of Ch 2 §2.1. We depart from xv6 significantly on address
> space management: xv6 uses a flat `sz` field; we introduce VMAs.

---

## Part 1: The Address Space Problem

### What fork and exec need to do

**fork**: create an exact duplicate of the calling process's memory —
every byte of code, data, stack, and heap in the child is a copy of
the parent's.

**exec**: throw away the current process's memory entirely and replace
it with a new program loaded from an ELF binary.

Both operations need to answer the same question: **which regions of
virtual memory does this process own?**

### The naive answer: a single number

xv6 uses a `uint64 sz` field — "the process occupies addresses
`[0..sz)`." fork copies every valid page in that range. exec frees
them all and starts fresh. Simple.

We already have `p->sz = 0x4000` in our test process. But this model
has a structural problem: it assumes the address space is a single
contiguous blob from 0 to sz. That breaks when:

- The stack lives at a high address (not adjacent to text)
- Multiple threads need separate stacks (Phase 9)
- Future mmap allocates memory at arbitrary addresses (stretch goal)
- COW fork needs to track permissions per region (Round 6-4)

We chose to put the stack at a high address (below TRAPFRAME) rather
than sandwiched between text and heap. This gives each region room to
grow independently — but it means `sz` can't describe the address
space anymore.

### The solution: Virtual Memory Areas (VMAs)

A VMA is a descriptor for one contiguous region of the address space:

```c
struct vma {
    uint64 start;          // first byte (page-aligned)
    uint64 end;            // one past last byte (page-aligned)
    int perm;              // intended permissions: PTE_R | PTE_W | PTE_X | PTE_U
    list_head link;        // linked into proc->vma_list
};
```

Each process owns a list of VMAs:

```
proc->vma_list:
    VMA [0x1000, 0x3000)  perm=RX|U   "text + rodata"
    VMA [0x3000, 0x4000)  perm=RW|U   "data + bss"
    VMA [HEAP_START, HEAP_START) perm=RW|U "heap" (start==end, zero pages until sbrk grows it)
    VMA [STACK_BOT, STACK_TOP)  perm=RW|U  "stack"
```

Now every operation has a clear answer:

| Operation | With `sz` | With VMAs |
|-----------|-----------|-----------|
| fork — which pages to copy? | Walk `[0..sz)`, check PTEs | Iterate VMA list, copy each region |
| exec — which pages to free? | Free `[0..sz)` | Free each VMA's region |
| page fault — is this address valid? | `addr < sz`? | Is there a VMA containing addr? |
| sbrk — grow the heap | Bump `sz` | Extend the heap VMA's `end` |
| mmap (future) | Doesn't fit | Add a new VMA |
| COW — which regions are writable? | Can't tell (one global `sz`) | Check `vma->perm & PTE_W` |

### VMA vs PTE permissions

A subtle but important distinction: the VMA's `perm` field records
**intended permissions** — what the process is *allowed* to do with
this region. The PTEs in the page table record **current hardware
permissions** — what the MMU will permit right now.

These can differ:
- **COW fork (Round 6-4)**: a writable VMA has its PTEs set to
  read-only. When the process writes, the page fault handler sees
  "VMA says writable, PTE says read-only" → COW copy.
- **Lazy allocation**: a VMA exists but has no pages mapped yet. The
  page fault handler allocates on demand.
- **Guard pages**: deliberately unmapped pages within a VMA's range
  for stack overflow detection.

For Round 6-3, VMA perm and PTE perm are always identical (no COW,
no lazy alloc). But designing the VMA to hold intended permissions
means Round 6-4 is a local change to the page fault handler — not a
restructuring of the VMA abstraction.

### Memory layout with VMAs

Our user address space becomes:

```
┌──────────────────────────────────┐ ← 0x40_0000_0000 (MAX_VA)
│ TRAMPOLINE (1 page, R-X)         │
├──────────────────────────────────┤ ← MAX_VA - PG_SIZE
│ TRAPFRAME  (1 page, R-W, no U)   │
├──────────────────────────────────┤ ← MAX_VA - 2*PG_SIZE
│ guard page (unmapped)            │
├──────────────────────────────────┤ ← USER_STACK_TOP (sp starts here)
│ user stack (1 page, RW+U)        │
├──────────────────────────────────┤ ← USER_STACK_TOP - PG_SIZE
│                                  │
│       (large unmapped gap)       │
│                                  │
├──────────────────────────────────┤
│ heap (grows upward, RW+U)        │
├──────────────────────────────────┤ ← end of data
│ data / bss (RW+U)                │
├──────────────────────────────────┤
│ text / rodata (RX+U)             │
├──────────────────────────────────┤ ← 0x1000
│ (unmapped — NULL deref zone)     │
└──────────────────────────────────┘ ← 0x0000_0000_0000
```

**USER_STACK_TOP** is placed at `TRAPFRAME - PG_SIZE` — one guard
page below the trapframe. The stack VMA occupies `[STACK_TOP - PG_SIZE,
STACK_TOP)` initially (one page). This gives the maximum gap between
heap and stack for future growth.

Note: TRAMPOLINE and TRAPFRAME are mapped without PTE_U — they aren't
user-accessible and aren't tracked by VMAs. VMAs only describe
user-accessible regions.

### The VMA operations we need

| Function | Purpose |
|----------|---------|
| `vma_create(start, end, perm)` | Allocate and initialize a VMA |
| `vma_add(proc, vma)` | Insert into process's VMA list (sorted by start) |
| `vma_find(proc, addr)` | Find VMA containing a given address |
| `vma_dup_all(dst, src)` | Deep-copy all VMAs + their pages (for fork) |
| `vma_free_all(proc)` | Free all VMAs + their mapped pages (for exec/exit) |

These are all short functions (10–25 lines each). The list is sorted
by start address — for our small number of regions (3–5 per process),
a linear scan in `vma_find` is perfectly adequate. (Linux uses an
intrusive red-black tree of VMAs for O(log n) lookup — processes
there can have hundreds of regions from shared libraries, mmap calls,
and thread stacks. We don't need that complexity.)

---

## Part 2: Reference-Counted Pages — page_get / page_put

### The problem

When fork copies a page, and later the child exits, the child frees
its copy. Simple. But in Round 6-4 (COW fork), parent and child
**share** the same physical page. When the child exits, should it free
the page? Only if the parent isn't still using it.

We need to track: **how many users does this physical page have?**

### The infrastructure we already have

Lecture 3-2 built `struct page` with a `refcount` field:

```c
struct page {
    uint16 refcount;    // 0 = free, ≥1 = allocated
    uint8  order;       // buddy allocator order
    uint8  flags;       // PG_SLAB, PG_BIG, etc.
    // ... slab union ...
};
```

`kalloc()` sets refcount = 1. `kfree()` asserts refcount == 1 and
sets it to 0. `pa_to_page(pa)` returns the struct page for any
physical address.

What's missing: helpers to **increment** and **decrement-with-free**.

### The two new helpers

```c
void page_get(void *pa) {
    struct page *pg = pa_to_page((uint64)pa);
    pg->refcount++;
}

void page_put(void *pa) {
    struct page *pg = pa_to_page((uint64)pa);
    if (pg->refcount <= 0)
        panic("page_put: refcount already 0");
    pg->refcount--;
    if (pg->refcount == 0) {
        pg->refcount = 1;  // kfree/buddy_free asserts refcount == 1
        kfree(pa);
    }
}
```

In Round 6-3 (deep-copy fork), every user page has refcount = 1. Calling
`page_put` decrements to 0 and frees — exactly like `kfree`. No
behavioral change yet.

In Round 6-4 (COW fork), fork calls `page_get` instead of copying.
Refcount goes to 2. When either process exits, `page_put` decrements
to 1 — doesn't free. Only the last user's exit frees the page.

### Where to call page_put

Every site that currently frees user pages (`kfree(pa)`) becomes
`page_put(pa)`:

- `vma_free_all` — freeing a process's entire address space (exit, exec)
- Explicit page deallocation in `sbrk` when shrinking (Round 6-4)

We never call raw `kfree` on user pages again. Kernel allocations
(kstack, proc struct, etc.) still use `kfree` directly — they're
never shared.

---

## Part 3: fork — Duplicating a Process

### What fork does (user perspective)

```c
int pid = fork();
if (pid == 0) {
    // child: new process, copy of parent
} else {
    // parent: pid = child's PID
}
```

One call, two returns. The child is an exact clone of the parent at the
moment of the fork — same memory contents, same register values, same
program counter (pointing at the instruction after `ecall`). The only
difference: fork returns 0 to the child and the child's PID to the
parent.

### Why fork + exec? Why not a single "spawn" syscall?

The Unix answer: **composition over configuration.**

A combined "spawn" syscall would need to accept every possible
environment modification as parameters: which file descriptors to
redirect, which directory to switch to, which signals to mask, which
resource limits to set. That's Windows `CreateProcess()`:

```c
// Windows — one monolithic call, ~10 parameters + nested structs
CreateProcess(
    "C:\\app.exe",          // program
    cmdline,                // arguments
    &sec_attrs,             // security for process
    &thread_sec_attrs,      // security for thread
    TRUE,                   // inherit handles?
    CREATE_NEW_CONSOLE,     // creation flags
    environment,            // environment block
    "C:\\workdir",          // working directory
    &startup_info,          // STARTUPINFO (stdin/stdout/stderr, window size, ...)
    &proc_info              // output: handles
);
```

Every new feature (redirect a handle, set CPU affinity, configure a
job object) adds parameters or nested structures. The API grows over
decades; callers pass NULL for the 8 fields they don't use.

Unix separates the problem into orthogonal steps:

```c
// Unix — composition of simple operations
int pid = fork();           // 1. get a clone
if (pid == 0) {
    close(1);              // 2. modify environment (close stdout)
    open("out.txt", ...);  // 3. modify environment (fd 1 → file)
    chdir("/tmp");         // 4. modify environment (working dir)
    exec("ls", argv);     // 5. become the new program
}
```

Each step uses ordinary syscalls — the same `close`, `open`, `chdir`
you'd use for anything else. No special "spawn flags." The shell
implements `>`, `|`, and `&` entirely in user space using this pattern:

| Shell syntax | What happens between fork and exec |
|---|---|
| `cmd > file` | `close(1); open(file)` — redirect stdout |
| `cmd < file` | `close(0); open(file)` — redirect stdin |
| `cmd1 \| cmd2` | `pipe(fds); fork(); dup2(...)` — connect stdout to stdin |
| `cd dir && cmd` | `chdir(dir)` — change working directory |
| `cmd &` | Parent doesn't `wait()` — child runs independently |

None of this requires kernel support beyond the basic primitives.
The kernel doesn't know what `>` means. The shell composes fork +
ordinary syscalls + exec to achieve redirection. This is why the Unix
shell is so powerful with so few kernel mechanisms.

> **The cost and the fix:**
>
> fork copies the entire address space, then exec throws it all away.
> Wasteful. Three responses exist:
>
> - **COW fork** (Round 6-4): pages are shared read-only, never
>   actually copied if exec comes quickly. fork+exec copies zero pages.
> - **vfork** (historical): share the parent's address space directly,
>   parent blocks until child calls exec. Fast but dangerous — child
>   can corrupt parent's stack. COW made this mostly obsolete.
> - **posix_spawn** (compromise): a library function that internally
>   uses vfork+exec with a restricted set of "file actions" for
>   redirections. Safer than raw vfork, faster than fork on systems
>   without COW (embedded/MMU-less).
>
> With COW, the performance argument for spawn disappears. The design
> clarity of fork+exec remains.

### What the kernel does

1. **Allocate a new proc struct** (like proc_create_kernel, but for user)
2. **Allocate kernel stack** and trapframe page
3. **Create an empty user page table** (proc_pagetable — maps trampoline + trapframe)
4. **Copy the parent's VMA list** — for each VMA, allocate fresh pages,
   memcpy the contents, map into child's page table
5. **Copy the trapframe** — child resumes at the same PC with the same
   registers
6. **Set child's trapframe->a0 = 0** — this is how fork returns 0 to the child
7. **Set parent-child links** — child->parent = current, add to children list
8. **Mark child RUNNABLE** — scheduler can pick it up
9. **Return child's PID to parent** (via normal syscall return path)

### Step 4 in detail: vma_dup_all

```c
int vma_dup_all(struct proc *dst, struct proc *src) {
    struct vma *v;
    list_for_each_entry(v, &src->vma_list, link) {
        // Create matching VMA in child
        struct vma *nv = vma_create(v->start, v->end, v->perm);
        if (!nv)
            goto fail;

        // Copy each page in this region
        for (uint64 va = v->start; va < v->end; va += PG_SIZE) {
            pte_t *pte = walk(src->pagetable, va, 0);
            if (!pte || !(*pte & PTE_V))
                continue;  // unmapped page within region (guard page)

            void *new_page = kalloc();
            if (!new_page)
                goto fail;

            uint64 pa = pte_to_pa(*pte);
            memcpy(new_page, (void *)pa, PG_SIZE);

            if (map_pages(dst->pagetable, va, PG_SIZE,
                          (uint64)new_page, pte_flags(*pte)) < 0) {
                kfree(new_page);
                goto fail;
            }
        }
        vma_add(dst, nv);
    }
    return 0;

fail:
    vma_free_all(dst);  // clean up partial copy
    return -1;
}
```

Key points:
- We iterate parent's VMAs (not a flat range)
- We use the PTE flags from the parent's page table (not the VMA perm)
  — in Round 6-3 these are identical, but in Round 6-4 with COW they
  may differ
- We skip invalid PTEs within a VMA range (guard pages have no mapping)
- On failure, we clean up everything already allocated

### The fork syscall wrapper

```c
int64 sys_fork(void) {
    return proc_fork();
}
```

The real work lives in `proc_fork()` (kernel/proc.c) — it's a kernel
function, not directly tied to the syscall interface. This separation
matters: kernel code that needs to create processes (like spawning
init) can call `proc_fork` without going through the syscall path.

### Why the child returns 0

The child's trapframe is a copy of the parent's. When the child is
scheduled, it enters through `user_trap_ret` → `user_ret` → `sret`,
resuming at `trapframe->epc` (the instruction after ecall). The
return value of a syscall is `trapframe->a0`. We explicitly set the
child's `trapframe->a0 = 0` before making it runnable — overriding
the parent's return value.

The parent continues normally through the syscall return path with
`trapframe->a0 = child_pid` (set by the dispatch code from
`sys_fork`'s return value).

---

## Part 4: exec — Loading a New Program

### What exec does (user perspective)

```c
exec("init", argv);
// never returns on success — the old program is gone
// returns -1 on failure (bad binary, out of memory)
```

exec replaces the calling process's entire address space with a new
program. Same PID, same parent, same kernel stack — but completely
new user memory, new code, new stack. It's a fresh start inside the
same process shell.

### The ELF format

ELF (Executable and Linkable Format) is the standard binary format on
Unix systems. Our user programs are already compiled as ELF — the
linker (`user.ld`) produces them. We just haven't parsed them yet.

An ELF file has this structure (left), compared with what
`objcopy -O binary` produced in Rounds 6-1/6-2 (right):

```
ELF file (full):                                    objcopy -O binary:
┌────────────────────────────────────────┐          ┌──────────────────┐
│ ELF Header                             │ stripped │                  │
│   magic, arch, entry point, phdr off   │ ──────── │                  │
├────────────────────────────────────────┤          │                  │
│ Program Headers                        │ stripped │                  │
│   [0] type, vaddr, memsz, filesz, flags│ ──────── │                  │
│   [1] ...                              │          │                  │
├────────────────────────────────────────┤          │                  │
│ .text bytes                            │   kept   │ raw instructions │
│ .rodata bytes                          │ ───────→ │ + rodata + data  │
│ .data bytes                            │   kept   │ (flat blob)      │
│ (.bss — no file space, memsz > filesz) │          │                  │
├────────────────────────────────────────┤          │                  │
│ Section headers / symbol table         │ stripped │                  │
│   (linker & debugger metadata)         │ ──────── │                  │
└────────────────────────────────────────┘          └──────────────────┘
```

The flat blob could be memcpy'd to VA 0x1000 with no parsing, but we
lost critical information: where each segment should be loaded, what
permissions it should have, and where the entry point is (all
hardcoded).

Now we skip the `objcopy -O binary` step. We embed the full ELF and
let exec parse the headers at runtime. What exec cares about:

1. **ELF Header** — validates the binary (magic number, architecture),
   gives us the entry point (`e_entry`) and where to find program headers
2. **Program Headers** — each `PT_LOAD` segment describes one chunk of
   memory to load: where in the file, where in virtual memory, how many
   bytes, what permissions

We don't need section headers, symbol tables, or debug info — those
are for linkers and debuggers. The program headers are the "load
instructions."

### ELF Header (what we check)

```c
struct elf_header {
    uint8  magic[4];     // "\x7fELF"
    uint8  class;        // 2 = 64-bit
    uint8  data;         // 1 = little-endian
    uint8  version;      // 1
    // ... padding ...
    uint16 type;         // 2 = ET_EXEC (executable)
    uint16 machine;      // 0xF3 = EM_RISCV
    // ...
    uint64 entry;        // virtual address of _start
    uint64 phoff;        // offset of program header table in file
    // ...
    uint16 phentsize;    // size of each program header entry
    uint16 phnum;        // number of program headers
};
```

We validate: magic, class=64-bit, machine=RISC-V. We read: entry,
phoff, phnum.

> **Common `e_machine` values:** 0x03 = x86 (32-bit), 0x3E = x86-64,
> 0x28 = ARM (32-bit), 0xB7 = AArch64 (Apple Silicon / server ARM),
> 0xF3 = RISC-V. This is how the OS rejects a binary built for the
> wrong CPU — exec sees the wrong machine value and fails immediately.

### Program Header (what we load)

```c
// ELF64 program header (note: flags moved before offset in 64-bit format)
struct elf_phdr {
    uint32 type;        // PT_LOAD = 1 (the only type we care about)
    uint32 flags;       // PF_X=1, PF_W=2, PF_R=4
    uint64 offset;      // offset in file where segment data starts
    uint64 vaddr;       // virtual address to load at
    uint64 paddr;       // physical addr (ignored in user-space)
    uint64 filesz;      // bytes in file (may be < memsz for .bss)
    uint64 memsz;       // bytes in memory (includes zeroed .bss)
    uint64 align;       // alignment requirement
};
```

> **Other program header types:** PT_LOAD (1) is the only one that puts
> bytes into memory. Others are metadata: PT_INTERP (3) names the
> dynamic linker (e.g., `/lib/ld-linux.so`), PT_DYNAMIC (2) points to
> the dynamic linking table, PT_NOTE (4) holds build-id or vendor info,
> PT_GNU_STACK (0x6474e551) controls stack executability. Our exec
> skips anything that isn't PT_LOAD.

For each PT_LOAD segment:
- Allocate pages covering `[vaddr, vaddr + memsz)`
- Copy `filesz` bytes from the file at `offset` into those pages
- Zero the remaining `memsz - filesz` bytes (this is .bss)
- Map with appropriate permissions derived from `flags`

### The exec algorithm

```
exec(name, argv):
    1. Look up embedded binary by name → (data, size)
    2. Validate ELF header (magic, arch, type)
    3. Build a NEW page table (proc_pagetable — trampoline + trapframe)
    4. For each PT_LOAD program header:
         a. Create a VMA for [vaddr, vaddr + memsz) with ELF permissions
         b. Allocate physical pages for the region
         c. Copy file data (filesz bytes) into pages
         d. Zero remaining bytes (bss)
         e. Map pages in the new page table
    5. Allocate and map a user stack (one page at USER_STACK_TOP - PG_SIZE)
       Create a stack VMA
    6. Push argv strings and pointer array onto the stack
    7. On success:
         - Free the OLD page table and all old VMAs (vma_free_all)
         - Install the new page table and VMA list
         - Set trapframe->epc = ELF entry point
         - Set trapframe->sp = stack pointer (after argv setup)
         - Return argc (placed in a0 by the syscall return path)
    8. On failure at any step:
         - Free everything allocated so far
         - Return -1 (old process continues unchanged)
```

### Why exec builds the new page table first

If exec fails halfway through (bad ELF segment, out of memory), the
old process must continue running as if nothing happened. By building
a completely new page table and VMA list before touching the old ones,
failure is free — just free the partial new allocation and return -1.

Only on success do we swap: replace old with new, free old. This is
the "build new, then swap" pattern — like a database transaction.

### Permission mapping: ELF flags → PTE bits

```c
int elf_to_pte(uint32 flags) {
    int perm = PTE_U;
    if (flags & PF_R) perm |= PTE_R;
    if (flags & PF_W) perm |= PTE_W;
    if (flags & PF_X) perm |= PTE_X;
    return perm;
}
```

Typical ELF segments:
- `.text` → PF_R | PF_X → PTE_R | PTE_X | PTE_U (execute, read-only)
- `.data` + `.bss` → PF_R | PF_W → PTE_R | PTE_W | PTE_U (read-write)

Each segment becomes one VMA with matching permissions.

---

## Part 5: Embedding User Binaries

### The problem: no filesystem yet

exec needs to read an ELF binary by name. Phase 7 adds a filesystem;
until then, user binaries must be baked into the kernel image at
compile time. We need a mechanism to:

1. Embed arbitrary binary blobs into the kernel ELF
2. Look them up by name at runtime

### Current mechanism (Round 6-1/6-2)

The build flow for embedding the test user program:

```
user/test_user.S                    ← source (the actual user program)
    │  assemble + link (via user.ld)
    ▼
user/test_user.elf                  ← full ELF binary
    │  objcopy -O binary (strip headers — Makefile: $(OBJCOPY) -O binary $< $@)
    ▼
user/test_user.bin                  ← raw flat bytes
    │
    │  referenced by .incbin inside:
    ▼
kernel/arch/user_test_bin.S         ← glue: pastes bytes into kernel .rodata
    │  assemble into kernel
    ▼
kernel binary now contains the user program bytes
    (accessible via test_user_bin / test_user_bin_end symbols)
```

The `.incbin` wrapper (`kernel/arch/user_test_bin.S`) is minimal:

```asm
.section .rodata
.globl test_user_bin
test_user_bin:
    .incbin "user/test_user.bin"
.globl test_user_bin_end
test_user_bin_end:
```

It has no real code — just tells the assembler "paste these bytes into
the kernel's `.rodata` section" and exposes start/end symbols so C code
can find them.

### Generalization for multiple programs

Same technique, one file per program:

```asm
# kernel/arch/user_bin_init.S
.section .rodata
.globl _binary_init_start, _binary_init_end
_binary_init_start:
    .incbin "user/init"       # embed the ELF directly (not a .bin)
_binary_init_end:
```

We embed the full ELF (not a flat binary), because exec parses the
ELF headers itself.

### The lookup table

A simple array maps names to embedded data:

```c
struct embedded_binary {
    const char *name;
    const uint8 *start;
    const uint8 *end;
};

static struct embedded_binary binaries[] = {
    { "init",  _binary_init_start,  _binary_init_end },
    { "hello", _binary_hello_start, _binary_hello_end },
    // add entries as we add user programs
};

const struct embedded_binary *
lookup_binary(const char *name) {
    for (int i = 0; i < ARRAY_SIZE(binaries); i++)
        if (strcmp(binaries[i].name, name) == 0)
            return &binaries[i];
    return NULL;
}
```

exec calls `lookup_binary(name)` and gets a pointer to the ELF data
in kernel memory. When Phase 7 adds a filesystem, we replace this
function's implementation (read from disk instead of the table) and
exec doesn't change.

### Build process

```makefile
# User programs: compile as ELF (not stripped to flat binary)
user/init: user/init.c user/user.ld
    $(CC) -nostdlib -T user/user.ld -o $@ $<

# Embed into kernel
kernel/arch/user_bin_init.o: user/init
    # ... or use the .S + .incbin approach
```

The user linker script (`user/user.ld`) already produces ELF with text
at 0x1000. We just stop the `objcopy -O binary` step — exec reads the
ELF directly.

---

## Part 6: argc/argv — Passing Arguments to Programs

### The problem

When init does `exec("hello", argv)`, the new program needs to receive
those arguments. The C runtime expectation:

```c
int main(int argc, char *argv[]) { ... }
```

`argc` is in register `a0`, `argv` is in `a1`. Both must be set up
by exec before jumping to `_start`.

### How it works: push onto the user stack

exec allocates a fresh stack page. Before setting `sp`, it pushes the
argument data onto the stack:

```
┌─────────────────────────┐ ← USER_STACK_TOP
│ "hello\0"               │   string bytes        ─┐
│ "/path\0"               │                        │ set up by exec,
├─────────────────────────┤                        │ never written again
│ alignment padding       │                        │ (main reads via argv
├─────────────────────────┤                        │  pointer, not by
│ argv[2] = NULL          │   pointer array        │  walking the stack)
│ argv[1] = ptr to "/path"│                        │
│ argv[0] = ptr to "hello"│                       ─┘
├─────────────────────────┤ ← sp (16-byte aligned)
│                         │                        ─┐
│  (room for local vars)  │   grows downward        │ compiler only
│                         │                        ─┘ writes at/below sp
└─────────────────────────┘ ← USER_STACK_TOP - PG_SIZE
```

The two regions never conflict: the compiler always moves `sp`
downward for new frames and local variables — it never writes above
the current `sp`. The argv metadata at the top stays intact because
`main` accesses it through the `argv` pointer (passed in `a1`), not
by indexing upward from `sp`.

### The algorithm

```
1. Start sp at USER_STACK_TOP (top of stack page)
2. For each argument string (right to left):
     - sp -= strlen(arg) + 1
     - copy string bytes to sp (via the physical page directly)
     - record this user VA in a temp array: uargv[i] = sp
3. Align sp down to 16 bytes (RISC-V calling convention)
4. sp -= (argc + 1) * 8     // space for pointer array + NULL
5. For i = 0..argc-1:
     - write uargv[i] to stack at sp + i*8
6. Write NULL at sp + argc*8  // argv[argc] = NULL
7. Set trapframe->sp = sp
8. Set trapframe->a0 = argc     // returned by exec (also main's first arg)
9. Set trapframe->a1 = sp       // points to argv[0] on the stack
```

### Why push strings first, then pointers

The pointers in argv[] point to the strings. We need to know where the
strings land (their user VAs) before we can write the pointers. So
strings go on the stack first (at higher addresses), then the pointer
array below them.

### Stack overflow check

exec checks that the argument data fits within one page. If the total
size of all argv strings + pointers exceeds the stack page, exec fails
with -1. Real OSes (Linux) allow much larger argument lists (128 KB+),
but one page is plenty for our simple programs.

---

## Part 7: The Remaining Syscalls

### sys_fork

A thin wrapper — calls `proc_fork()` which does all the work
described in Part 3. Returns child PID to parent; 0 is written to
child's trapframe->a0 directly.

### sys_exec

```c
int64 sys_exec(void) {
    struct proc *p = this_proc();
    uint64 upath = p->trapframe->a0;
    uint64 uargv = p->trapframe->a1;

    // Copy path string from user space
    char path[64];
    if (copyinstr(p->pagetable, path, upath, sizeof(path)) < 0)
        return -EFAULT;

    // Copy argv pointers and strings from user space
    char *argv[16];
    char kargv_buf[512];  // scratch space for all strings
    int argc = 0;
    int off = 0;
    for (; argc < 15; argc++) {
        uint64 uptr;
        if (copyin(p->pagetable, &uptr, uargv + argc * 8, 8) < 0)
            return -EFAULT;
        if (uptr == 0)
            break;
        argv[argc] = &kargv_buf[off];
        if (copyinstr(p->pagetable, argv[argc], uptr, sizeof(kargv_buf) - off) < 0)
            return -EFAULT;
        off += strlen(argv[argc]) + 1;
    }
    argv[argc] = NULL;

    return proc_exec(path, argv);
}
```

`proc_exec(path, argv)` operates on the current process (`this_proc()`).
Internally, `init_user_proc` needs to load an ELF into a *specified*
process (not necessarily the current one), so it calls a lower-level
`proc_exec_internal(p, name, argv)`. The syscall wrapper calls
`proc_exec` which is just `proc_exec_internal(this_proc(), path, argv)`.

### copyinstr — a new helper

exec needs to copy null-terminated strings from user space. `copyin`
copies a fixed number of bytes; `copyinstr` copies until it finds a
null byte or hits a limit:

```c
int copyinstr(pte_t *pagetable, char *dst, uint64 srcva, uint64 max) {
    // Same page-at-a-time walk as copyin, but stops at '\0'
    // Returns 0 on success, -EFAULT on bad address,
    //         -ENAMETOOLONG if no null found within max bytes
}
```

### sys_wait

```c
int64 sys_wait(void) {
    struct proc *p = this_proc();
    uint64 user_status = p->trapframe->a0;

    int kstatus;
    int pid = proc_wait(&kstatus);

    if (pid > 0 && user_status != 0) {
        if (copyout(p->pagetable, user_status, &kstatus, sizeof(int)) < 0)
            return -EFAULT;
    }
    return pid;
}
```

The key difference from the kernel-internal `proc_wait`: the status
pointer is a user virtual address. We use `copyout` to safely write
the exit status to user memory.

### sys_getpid

```c
int64 sys_getpid(void) {
    return this_proc()->pid;
}
```

Trivial — no arguments, no memory access. Returns the current
process's PID directly.

### Updated syscall table

```c
#define SYS_write   1
#define SYS_exit    2
#define SYS_fork    3
#define SYS_exec    4
#define SYS_wait    5
#define SYS_getpid  6
#define SYS_kill    7
#define NSYSCALL    8

static int64 (*syscalls[])(void) = {
    [0]          = NULL,
    [SYS_write]  = sys_write,
    [SYS_exit]   = sys_exit,
    [SYS_fork]   = sys_fork,
    [SYS_exec]   = sys_exec,
    [SYS_wait]   = sys_wait,
    [SYS_getpid] = sys_getpid,
    [SYS_kill]   = sys_kill,
};
```

### sys_kill

```c
int64 sys_kill(void) {
    int pid = (int)this_proc()->trapframe->a0;
    return proc_kill(pid);
}
```

Wraps the existing `proc_kill(pid)` — sets the target's `killed`
flag. The target dies on its next return to user mode (checked in
user_trap_ret).

---

## Part 8: Completing free_proc — User Resource Cleanup

### The TODO we left in Round 5-2

```c
static void free_proc(struct proc *p) {
    list_del(&p->sibling);
    list_del(&p->all_list);
    list_del(&p->pid_link);
    /* TODO(Round 6-3): free user resources */
    kfree((void *)p->kstack);
    kmfree(p);
}
```

Now we can fill it in:

```c
static void free_proc(struct proc *p) {
    list_del(&p->sibling);
    list_del(&p->all_list);
    list_del(&p->pid_link);

    // Free user resources (if this is a user process)
    if (p->pagetable) {
        vma_free_all(p);                // free all user pages via page_put
        proc_free_pagetable(p->pagetable);  // free page table pages
        kfree((void *)p->trapframe);    // free trapframe page
    }

    kfree((void *)p->kstack);
    kmfree(p);
}
```

### vma_free_all

```c
void vma_free_all(struct proc *p) {
    struct vma *v, *tmp;
    list_for_each_entry_safe(v, tmp, &p->vma_list, link) {
        for (uint64 va = v->start; va < v->end; va += PG_SIZE) {
            pte_t *pte = walk(p->pagetable, va, 0);
            if (pte && (*pte & PTE_V)) {
                page_put((void *)pte_to_pa(*pte));
                *pte = 0;
            }
        }
        list_del(&v->link);
        kmfree(v);
    }
}
```

This walks each VMA's range, frees every mapped physical page via
`page_put`, clears the PTE, then frees the VMA struct itself.

### proc_free_pagetable — freeing the page table pages themselves

After `vma_free_all` clears all leaf PTEs and frees user pages, the
page table still has its intermediate pages (level-2, level-1 tables)
and the root page. These are kernel-allocated pages (via `kalloc`)
that must also be freed:

```c
void proc_free_pagetable(pte_t *pagetable) {
    // Walk the 3-level tree, free intermediate pages
    // Don't free anything the leaf PTEs point to (already freed by vma_free_all)
    // Don't free trampoline/trapframe mappings' targets (they're shared/separate)
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && !(pte & (PTE_R | PTE_W | PTE_X))) {
            // Non-leaf entry — points to a lower-level table page
            uint64 child = pte_to_pa(pte);
            // Recurse one level (or iterate for 2 levels)
            proc_free_pagetable((pte_t *)child);
        }
    }
    kfree(pagetable);
}
```

A PTE is non-leaf if it's valid but has no R/W/X bits set — it points
to the next level of the page table. We recursively free those pages,
then the table page itself.

---

## Part 9: The First Real Init

### Refactoring process creation: alloc_proc

Currently `proc_create_kernel` does everything in one function:
allocate struct, assign PID, allocate kstack, init spinlock/waitqueue/
lists, set parent, add to scheduler. `proc_create_user_test` repeats
most of this. fork will need it too.

We factor out the common base into `alloc_proc`:

```c
static struct proc *alloc_proc(void) {
    struct proc *p = kmalloc(sizeof(struct proc));
    memset(p, 0, sizeof(struct proc));

    p->pid = alloc_pid();
    p->kstack = (uint64)kalloc();
    if (!p->kstack)
        panic("alloc_proc: kalloc failed");

    spin_init(&p->lock, "proc");
    wq_init(&p->child_wq, "proc");
    INIT_LIST_HEAD(&p->children);
    INIT_LIST_HEAD(&p->wait_link);
    INIT_LIST_HEAD(&p->vma_list);

    return p;
}
```

Now the three creation paths use it:

| Path | What it does after alloc_proc |
|------|-------------------------------|
| `proc_create_kernel(fn, name)` | Set context.ra = fn, context.sp = kstack top. No pagetable/trapframe. |
| `proc_create_user()` | Allocate trapframe + pagetable. Return a user proc with empty address space. |
| `proc_fork()` | Call proc_create_user, then copy parent's VMAs + trapframe. Set child's a0 = 0. |

### Replacing proc_create_user_test

`proc_create_user` is the generic factory for user processes:

```c
struct proc *proc_create_user(void) {
    struct proc *p = alloc_proc();

    p->trapframe = (struct trapframe *)kalloc();
    memset(p->trapframe, 0, PG_SIZE);
    p->pagetable = proc_pagetable(p);

    // context.ra = user_proc_start (stub that calls user_trap_ret)
    p->context.ra = (uint64)user_proc_start;
    p->context.sp = p->kstack + PG_SIZE;

    return p;
}
```

It returns a user process with an empty address space — no VMAs, no
user pages mapped. The caller fills it in:

```c
// Boot: create init (PID 1)
void init_user_proc(void) {
    struct proc *p = proc_create_user();
    proc_exec_internal(p, "init", NULL);  // load ELF into empty address space

    p->parent = NULL;
    init_proc = p;
    p->state = PROC_RUNNABLE;
    list_add_tail(&p->all_list, &all_procs);
    run_queue_add(p);
    hash_add(pid_table, &p->pid_link, PID_HASH_BITS, hash_int(p->pid));
}
```

This is the ONE place the kernel directly creates a user process
without fork. Once init runs, everything else happens through
fork+exec. `proc_fork` also calls `proc_create_user`, then copies
the parent's VMAs instead of loading an ELF.

### The init program

```c
// user/init.c
#include "user.h"

int main(void) {
    for (;;) {
        int pid = fork();
        if (pid == 0) {
            // Child: exec the hello program
            char *argv[] = { "hello", 0 };
            exec("hello", argv);
            // exec failed
            exit(1);
        }
        // Parent: wait for any child
        int status;
        wait(&status);
    }
}
```

### The hello program

```c
// user/hello.c
#include "user.h"

int main(int argc, char *argv[]) {
    write(1, "hello world\n", 12);
    exit(0);
}
```

### User-side syscall stubs

User programs need thin wrappers to invoke syscalls. A minimal
`user/usys.S` (or generated via macros):

```asm
.globl fork
fork:
    li a7, 3        # SYS_fork
    ecall
    ret

.globl exec
exec:
    li a7, 4        # SYS_exec
    ecall
    ret

.globl wait
wait:
    li a7, 5        # SYS_wait
    ecall
    ret

# ... etc for write, exit, getpid, kill
```

And a minimal `_start` that calls main:

```asm
.globl _start
_start:
    call main
    # main returned — exit with its return value (in a0)
    li a7, 2        # SYS_exit
    ecall
```

### What happens at boot

```
1. Kernel boots, initializes everything
2. init_user_proc() — loads init ELF, marks runnable
3. Scheduler picks init (PID 1)
4. init calls fork() → child (PID 2) created
5. Child calls exec("hello") → address space replaced with hello ELF
6. hello: write(1, "hello world\n", 12) → prints to console
7. hello: exit(0) → becomes zombie
8. init: wait(&status) → reaps PID 2, status=0
9. init loops: fork again...
```

Console output:
```
hello world
hello world
hello world
...
```

Init forks and execs forever, each child prints and exits. The first
real process tree.

---

## Quick Reference

### New files

```
kernel/
    vma.c               # VMA operations: create, add, find, dup_all, free_all
    include/vma.h       # struct vma definition
    include/elf.h       # ELF header and program header structs

include/
    syscall_num.h       # updated: SYS_fork=3, SYS_exec=4, SYS_wait=5, etc.

user/
    init.c              # first user process: fork+exec+wait loop
    hello.c             # test program: write + exit
    usys.S              # syscall stubs (fork, exec, wait, write, exit, getpid)
    start.S             # _start: call main, then exit(a0)
    user.h              # syscall prototypes for user programs
    user.ld             # updated linker script (ELF with proper sections)

kernel/arch/
    user_bin_init.S     # .incbin user/init (embedded ELF)
    user_bin_hello.S    # .incbin user/hello (embedded ELF)
```

### Modified files

```
kernel/proc.c           # alloc_proc, proc_create_user, proc_fork, free_proc (complete)
kernel/vm.c             # page_get, page_put, copyinstr, proc_free_pagetable
kernel/syscall.c        # sys_fork, sys_exec, sys_wait, sys_getpid, sys_kill
kernel/include/proc.h   # remove sz, add vma_list to struct proc
kernel/include/kalloc.h # declare page_get, page_put
kernel/include/mem_layout.h  # USER_STACK_TOP constant
Makefile                # build user programs as ELF, embed into kernel
```

### Syscall table (Round 6-3)

| Number | Name | Args | Returns |
|--------|------|------|---------|
| 1 | write | fd, buf, len | bytes written or -error |
| 2 | exit | status | never returns |
| 3 | fork | (none) | child PID to parent, 0 to child |
| 4 | exec | path, argv | argc on success, -1 on failure |
| 5 | wait | status_ptr | child PID or -1 |
| 6 | getpid | (none) | current PID |
| 7 | kill | pid | 0 or -1 |

### bobchouOS vs xv6

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Address space tracking | `uint64 sz` (flat) | VMA list (per-region) |
| Stack placement | After text, below heap | High address, below TRAPFRAME |
| Binary format in exec | ELF (from filesystem) | ELF (from embedded table) |
| Page cleanup | uvmfree: walk `[0..sz)` | vma_free_all: iterate VMA list |
| Physical page lifetime | bare kfree | page_put (refcount-aware) |
| fork implementation | uvmcopy: walk `[0..sz)` | vma_dup_all: iterate VMA list |
| argc/argv | Pushed onto stack by exec | Same approach |
| Binary source | inode (filesystem) | lookup_binary (embedded table) |
| Process creation | userinit (inline initcode[]) | proc_create_user + proc_exec_internal |
| wait semantics | wait(addr) | sys_wait(user_status) + copyout |

### VMA operations

| Function | Signature | Purpose |
|----------|-----------|---------|
| vma_create | `(start, end, perm) → vma*` | Allocate + init |
| vma_add | `(proc, vma)` | Insert sorted by start |
| vma_find | `(proc, addr) → vma*` | Lookup by address |
| vma_dup_all | `(dst, src) → int` | Deep-copy for fork |
| vma_free_all | `(proc)` | Free all regions + pages |

### Memory layout constants

| Name | Value | Purpose |
|------|-------|---------|
| USER_TEXT_START | 0x1000 | First page of user code |
| USER_STACK_TOP | TRAPFRAME - PG_SIZE | Top of stack region |
| TRAPFRAME | MAX_VA - 2*PG_SIZE | Trapframe page VA |
| TRAMPOLINE | MAX_VA - PG_SIZE | Trampoline page VA |
