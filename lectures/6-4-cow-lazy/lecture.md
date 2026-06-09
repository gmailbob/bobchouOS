# Lecture 6-4: sbrk, Copy-on-Write, and Demand Paging

> **Where we are**
>
> Round 6-3 built the process lifecycle: fork duplicates an address
> space page-by-page, exec loads a fresh program from ELF, wait/kill/
> sleep manage the family tree, and init forks+execs a child end-to-end.
>
> But our fork is expensive — it physically copies every page of the
> parent even though the child often calls exec() immediately and throws
> those copies away. And user programs have no way to allocate heap
> memory at runtime: the text and stack are fixed at exec time.
>
> This round introduces three mechanisms that make our virtual memory
> system practical:
>
> 1. **sbrk()** — grow and shrink the process heap at runtime
> 2. **Copy-on-write (COW) fork** — share pages between parent and child
>    until one writes, then copy on demand
> 3. **Demand paging (lazy allocation)** — don't allocate physical pages
>    until the process actually touches them
>
> All three share a single enabler: **the page fault handler**. When the
> CPU tries to access a page that is either unmapped or read-only-but-
> logically-writable, the hardware traps into the kernel. We inspect
> the VMA, determine why the fault occurred, and handle it — allocating
> a fresh page, copying a COW page, or killing the process for an
> illegal access. This is the core mechanic of modern virtual memory.
>
> **What you will understand after this lecture:**
>
> - How sbrk() manipulates the heap VMA (grow and shrink)
> - The COW protocol: page_get (increment refcount) on fork, PTE_COW marker, copy-on-write fault
> - Why demand paging improves both performance and memory usage
> - Page fault dispatch: reading scause and stval, routing to handlers
> - The unified fault handler: a decision tree for COW / lazy / illegal
> - How refcounts determine whether to copy or simply remap
> - Interaction between these mechanisms and exec/fork
> - How copyout/copyin must pre-fault lazy pages

> **xv6 book coverage:**
> This lecture covers Ch 4 §4.6 ("Page faults"), the COW lab design,
> and the lazy-allocation lab. We go beyond xv6's approach by unifying
> all three into a single fault handler driven by VMA metadata.

---

## Part 1: The Heap Problem

### Programs need dynamic memory

After exec, a process has fixed regions: text (code), data (globals),
and stack. But real programs allocate memory at runtime — linked lists,
buffers, parsed input. In Unix, the mechanism is `sbrk(n)` — "set break":

```c
old_break = sbrk(n);   // grow heap by n bytes, return old break
sbrk(-n);              // shrink heap by n bytes
```

The "program break" is the boundary between mapped heap and unmapped
space. sbrk moves it up (allocate) or down (free). User-space malloc
calls sbrk internally to get pages from the kernel, then carves those
pages into smaller allocations.

### Where does the heap live?

After exec loads PT_LOAD segments, the highest mapped data address is
the end of the last writable segment (typically .bss). The heap starts
immediately above that, page-aligned:

```
 Low VA                                                        High VA
 ┌──────────┬────────────┬─── ... ───┬───────┬────────────┬──────────┐
 │  text    │  data/bss  │   HEAP    │ guard │   STACK    │ trapframe│
 │ 0x1000   │            │  ← brk →  │ (1pg) │  (≤16 pg)  │          │
 └──────────┴────────────┴─── ... ───┴───────┴────────────┴──────────┘
                         ^           ^       ^            ^
                     heap_start   HEAP_MAX  stack_bot  USER_STACK_TOP
```

We track the heap with a dedicated VMA. Initially it has
`start == end` (zero size). sbrk grows `end` upward; shrink moves it
back down.

### Finding the heap: VMA flags

Rather than storing a `heap_start` field in `struct proc`, we mark
the heap VMA with a type flag. This requires one new field on
`struct vma`:

```c
struct vma {
    uint64 start;
    uint64 end;
    int perm;              /* PTE permissions (existing) */
    uint32 flags;          /* VMA_HEAP, VMA_STACK, etc. (new) */
    struct list_head link;
};

#define VMA_HEAP   (1 << 0)
#define VMA_STACK  (1 << 1)
```

sbrk finds the heap via `vma_find_by_flags(p, VMA_HEAP)` — a linear
scan of the process's VMA list returning the first entry whose flags
match. The heap
VMA's `start` is the immutable floor (the program break can never
shrink below it), and `end` is the current break. One source of truth,
no sync invariant to maintain.

exec creates the heap VMA after loading all segments:

```c
uint64 heap_start = 0;
for (each PT_LOAD segment) {
    uint64 seg_end = PG_ROUND_UP(phdr->p_vaddr + phdr->p_memsz);
    if (seg_end > heap_start)
        heap_start = seg_end;
}

struct vma *heap_vma = vma_create(heap_start, heap_start,
                                  PTE_R | PTE_W | PTE_U, VMA_HEAP);
```

We use `p_memsz` (not `p_filesz`) because the segment's in-memory
footprint includes .bss — uninitialized globals that don't occupy file
space but do occupy virtual address space (see Lecture 6-3, Part 4).

The ELF spec guarantees PT_LOAD segments do not overlap in virtual
address space, so taking `max(seg_end)` across all segments correctly
finds the first free address above all program data.

`vma_create` now takes a fourth argument — `flags`. Text and data
VMAs pass `0`; the heap passes `VMA_HEAP`; the stack passes
`VMA_STACK`.

fork copies VMAs via `vma_dup_all`, which copies the flags field —
the child inherits the heap identity for free.

> **How Linux does it**
>
> Linux uses both approaches: each `vm_area_struct` has a `vm_flags`
> bitmask (VM_READ, VM_WRITE, VM_GROWSDOWN for stacks, etc.), AND the
> `mm_struct` caches `start_brk` and `brk` as direct fields.
>
> Why the redundancy? Linux processes can have thousands of VMAs
> (shared libraries, mmap regions, thread stacks, JIT zones). They
> live in a maple tree (formerly red-black tree) — there's no index by
> flag type. A scan-by-flag would be O(n). Caching `brk` makes the
> common bounds check O(1) without touching the tree.
>
> There's also an ordering optimization: `brk()` can reject invalid
> requests (`new_brk < start_brk`) before acquiring the mmap semaphore
> and performing any VMA lookup. The fast-reject path is a single
> pointer comparison.
>
> For us: 3–5 VMAs in a linked list, no lock contention (single hart).
> The flag-based lookup touches at most 5 nodes. Caching would add a
> sync invariant with no performance gain, so we keep a single source
> of truth in the VMA.

---

## Part 2: The sbrk Syscall

### Interface

```c
// SYS_sbrk = 9
int64 sys_sbrk(void) {
    int64 n = (int64)this_proc()->trapframe->a0;
    return proc_sbrk(n);
}
```

### proc_sbrk implementation

The real work lives in `proc_sbrk(int64 n)`:

Our break is always page-aligned — `vma->end` is the break, and we
round all adjustments to page boundaries. This simplifies the kernel
(no sub-page tracking) at the cost of slightly coarser granularity.
User-space malloc handles sub-page allocations internally.

**Growing (n > 0):**
1. Find the heap VMA via `vma_find_by_flags(p, VMA_HEAP)`
2. Compute `new_end = PG_ROUND_UP(vma->end + n)`
3. Check `new_end` doesn't collide with the stack region
4. Extend `vma->end = new_end`
5. **Don't allocate pages** — demand paging handles it on first touch
6. Return the old break address (the value `vma->end` had before)

**Shrinking (n < 0):**
1. Find the heap VMA
2. Compute `new_end = PG_ROUND_DOWN(vma->end + n)` (n is negative)
3. Check `new_end >= vma->start` (can't shrink below floor)
4. For each page in the released range: unmap PTE, call `page_put` (free page if last reference, else decrement refcount)
5. Shrink `vma->end = new_end`
6. Return the old break address

### Stack collision guard

We define a maximum stack size of 16 pages (64 KB) via
`STACK_MAX_PAGES`. The heap must never reach this reserved region:

```c
#define STACK_MAX_PAGES  16
#define HEAP_MAX  (USER_STACK_TOP - (STACK_MAX_PAGES + 1) * PG_SIZE)

if (new_end > HEAP_MAX)
    return -ENOMEM;
```

The `+1` is a guard page between heap and stack — it has no VMA, so
any access there is a clean segfault. The boundary is static: even if
the stack hasn't grown to its full 16 pages yet, the heap cannot
encroach into the reserved region. There's no reason to make this
dynamic — under Sv39 we have 256 GB of user virtual address space, so
reserving 64 KB for the stack costs nothing. A simple constant
comparison is all we need.

> **How Linux prevents heap–stack collision**
>
> Linux can't use a static constant because its layout is dynamic.
> ASLR (Address Space Layout Randomization) places the stack, heap,
> and mmap region at different random offsets each time a program runs.
> This defeats exploits that rely on hardcoded addresses — an attacker
> who finds a buffer overflow can't predict where the stack or
> shellcode will land. But it means the kernel can't know the heap
> ceiling at compile time; it must compute boundaries relative to
> whatever random layout was chosen.
>
> On top of ASLR, the mmap region sits between heap and stack, and the
> stack limit is user-configurable (`RLIMIT_STACK`, default 8 MB).
> Linux uses two-sided runtime checks:
>
> - **Heap side**: `brk()` rejects requests that would collide with the
>   mmap region above it (which in turn sits below the stack).
> - **Stack side**: when the stack faults downward, `expand_stack()`
>   checks that growth won't exceed `RLIMIT_STACK` and won't collide
>   with the next VMA below it. A 1 MB "stack guard gap" below the
>   stack VMA provides extra buffer.
>
> This is necessary for a general-purpose OS with ASLR, thread stacks
> at arbitrary addresses, and user-configurable limits. For us — fixed
> layout, no mmap region, no ASLR — the static constant is the right
> level of complexity.

### Shrink must unmap carefully

When shrinking, some pages in the released range may never have been
faulted in (they were lazily promised but never touched). Those have
no PTE — we skip them:

```c
for (uint64 va = new_end; va < old_end; va += PG_SIZE) {
    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_V)) {
        page_put((void *)pte_to_pa(*pte));
        *pte = 0;  // clear leaf PTE only — intermediate pages stay
    }
}
sfence_vma();
```

The `sfence.vma` flushes stale TLB entries for the unmapped pages.

We only zero the leaf PTE (the L0 entry pointing to the physical
page). The intermediate page table pages (L1, L2) remain allocated.
This is intentional: if the process later `sbrk`s back up to cover
the same VA, `walk()` follows the same path (VA bits deterministically
index each level) and finds the slot ready — no need to re-allocate
intermediate pages. They're freed at process exit when
`proc_free_pagetable` tears down all levels.

---

## Part 3: Copy-on-Write Fork

### The problem with deep-copy fork

Round 6-3's fork copies every physical page of the parent:

```
Parent has 10 pages → fork allocates 10 new pages → memcpy each one
Child immediately calls exec → frees all 10 copies
```

This is doubly wasteful: the allocation cost AND the copy cost are
thrown away. Shell-like programs fork+exec for every command — this
pattern dominates real workloads.

The classic fix: **copy-on-write**. Share pages and only copy when
someone writes.

### VMA perm vs PTE flags

Before diving into the protocol, a key distinction: the VMA `perm`
field records the **intended** permissions for a region. The PTE flags
are what the **hardware currently enforces**. Both use the same bit
constants (PTE_R, PTE_W, PTE_X, PTE_U), and normally they match —
COW is the only case where they diverge:

| Situation | VMA perm | PTE flags |
|-----------|----------|-----------|
| Text (normal) | R\|X\|U | R\|X\|U |
| Data/heap (normal) | R\|W\|U | R\|W\|U |
| After COW fork | R\|W\|U | R\|U\|COW (W cleared) |
| After COW resolved | R\|W\|U | R\|W\|U (restored) |

The VMA is the source of truth for "what should this page be." The
PTE is "what the hardware allows right now." The fault handler
consults the VMA to decide whether an access was *supposed* to be
legal, then fixes the PTE accordingly.

### The COW protocol

**On fork:**
1. Don't allocate new pages. Instead, map the child's PTEs to the
   SAME physical pages as the parent.
2. Mark BOTH parent's and child's PTEs as **read-only**, even for
   pages that are logically writable (VMA says PTE_W).
3. Set a software-defined bit (PTE_COW) on those downgraded PTEs
   so the fault handler knows this is COW, not a real read-only page.
4. Increment the physical page's refcount via `page_get` (1 → 2).

**On write (page fault):**
1. A store instruction (`sd`, `sw`, `sb`) hits a COW page → the MMU
   checks PTE_W as part of normal address translation → cleared → trap
   (scause=15). Load instructions (`ld`, `lw`, `lb`) check PTE_R
   instead — that's still set, so reads succeed with no trap. COW is
   zero-cost until a write happens: the permission check that triggers
   the fault is work the hardware does on every memory access anyway.
2. Fault handler reads `stval` (faulting address), finds the VMA
3. VMA says writable + PTE has PTE_COW → this is a COW fault
4. Check refcount:
   - If refcount == 1 (sole owner): just clear PTE_COW, set PTE_W.
     No copy needed — we're the only process using this page.
   - If refcount > 1: allocate a new page, memcpy the old page's
     contents, map the new page with full permissions, page_put the
     old page (decrement refcount).
5. Flush TLB (`sfence.vma`), resume execution. The faulting store
   instruction re-executes and succeeds.

### The PTE_COW bit

RISC-V PTE bits 8–9 are "RSW" — Reserved for Software. The hardware
ignores them; traps fire based on R/W/X only. We use bit 8:

```c
#define PTE_COW  (1L << 8)   /* RSW bit 0: copy-on-write marker */
```

The bit layout now looks like this:

```
 Bit:  9    8    7   6   5   4   3   2   1   0
      RSW1 COW   D   A   G   U   X   W   R   V
       │    │    │   │   │   │   │   │   │   │
       │    │    │   │   │   │   │   │   │   └─ Valid
       │    │    │   │   │   │   │   │   └───── Readable
       │    │    │   │   │   │   │   └───────── Writable
       │    │    │   │   │   │   └───────────── Executable
       │    │    │   │   │   └───────────────── User-accessible
       │    │    │   │   └───────────────────── Global
       │    │    │   └───────────────────────── Accessed
       │    │    └───────────────────────────── Dirty
       │    └────────────────────────────────── COW (software)
       └─────────────────────────────────────── (free)
```

### The COW fork loop

Replace the deep-copy loop in `proc_fork`:

```c
// Round 6-3 (deep copy):
for (va = vma->start; va < vma->end; va += PG_SIZE) {
    void *new_pg = kalloc();
    memcpy(new_pg, old_pg, PG_SIZE);
    map_pages(child_pt, va, PG_SIZE, new_pg, perm);
}

// Round 6-4 (COW):
for (va = vma->start; va < vma->end; va += PG_SIZE) {
    pte_t *parent_pte = walk(parent->pagetable, va, 0);
    if (!parent_pte || !(*parent_pte & PTE_V))
        continue;   // lazy page not yet faulted in — skip (see "Lazy + COW interaction" below)

    uint64 pa = pte_to_pa(*parent_pte);
    int flags = pte_flags(*parent_pte);

    // Downgrade writable pages to read-only + COW
    if (flags & PTE_W) {
        flags = (flags & ~PTE_W) | PTE_COW;
        *parent_pte = pa_to_pte(pa) | flags;  // parent PTE also changes
    }

    map_pages(child->pagetable, va, PG_SIZE, pa, flags);
    page_get((void *)pa);  // refcount: 1 → 2
}
sfence_vma();  // flush parent's stale writable TLB entries
```

Key points:
- Pages that are already read-only (text segments) stay as-is — no
  COW bit needed since nobody can write to them.
- Only writable pages get downgraded. The PTE_COW bit marks the
  downgrade so the fault handler can distinguish "was writable" from
  "was always read-only."
- The parent's TLB must be flushed because its PTEs changed from W
  to R. Without the flush, a cached TLB entry could allow a write
  without trapping.
- **Both** parent and child pay for their first write — each side's
  PTE is read-only, so whichever writes first takes a fault. But the
  cost depends on refcount: if the other side has already exited or
  exec'd, refcount is 1, and `cow_copy` just flips the permission
  bit (no allocation, no memcpy). The actual copy only happens when
  both sides are still alive and sharing.

### Why refcount == 1 means "sole owner"

Consider the fork+exec pattern:
1. Parent has page P with refcount=1
2. Fork → refcount=2 (shared by parent and child)
3. Child calls exec → child frees its address space → page_put on P →
   refcount=1 again
4. Parent writes to P → fault → handler sees refcount==1 →
   just clear COW and restore PTE_W. No copy.

This is the fast path for the overwhelmingly common fork+exec pattern.
Only when both parent and child continue running and writing to
shared pages does the actual copy happen.

---

## Part 4: Demand Paging (Lazy Allocation)

### The idea

When sbrk grows the heap by 16 pages, the program might only touch
3 of them. Allocating all 16 upfront wastes memory. Instead:

1. sbrk only updates the VMA's `end` (bookkeeping)
2. No physical pages are allocated
3. No PTEs are created
4. When the process first touches a new heap page → page fault
5. Fault handler sees: VMA covers this address, no PTE exists →
   allocate a zeroed page, map it, resume execution

This is **demand paging** (or **lazy allocation**). The process
doesn't notice — it sees memory appear at the addresses sbrk
promised. The kernel defers real work until proven necessary.

### Scope: heap and stack only

We apply demand paging to **heap and stack pages**. exec continues
to eagerly load text and data from the ELF. Why not lazy exec too?

Lazy exec would mean: "the first instruction fetch from .text faults,
and the fault handler loads the right ELF bytes into the page." That
requires the fault handler to have access to the ELF file data during
fault time. With our current embedded binaries, that's possible — but
it creates throwaway code:

- The fault handler would need a pointer to the embedded ELF blob
- In Phase 7, we replace embedded binaries with filesystem loading
- The fault handler would need to read from disk instead — completely
  different interface (file descriptors, block I/O, buffer cache)
- The embedded-blob fault path gets deleted

Instead, we keep exec eager now. When Phase 7 introduces file-backed
pages, lazy exec falls out naturally:

| Round | Fault handler path | Backing source |
|-------|-------------------|----------------|
| 6-4 | Zero-fill (heap + stack) | None (fresh page) |
| 7-x | File-read (text/data) | Filesystem + buffer cache |
| 8-4 | mmap (anon + file-backed) | Zero-fill or filesystem |

Each round extends the fault handler with one new branch. No
rewriting — just growing.

### Lazy + COW interaction

A lazily-allocated page that hasn't been touched yet has NO PTE at
all. When we fork:
- The COW loop skips pages with no valid PTE (nothing to share)
- The VMA is still duplicated for the child (via `vma_dup_all`)
- If the child touches that address later, it faults and gets its own
  fresh zeroed page — no COW needed because no sharing happened

This falls out naturally from the "skip if PTE not valid" check in
the COW fork loop. No special cases needed.

### Elastic stack via demand paging

Round 6-3 gave each process a single stack page (4 KB). That's
tight — a few nested function calls with local buffers and you're
out. But allocating 16 pages upfront for every process wastes memory
when most never use more than 2–3.

The solution is the same as heap demand paging: **declare a large
stack VMA, but only back it with physical pages on demand.**

```c
#define STACK_MAX_PAGES  16   // 64 KB max stack

// In exec: create stack VMA spanning 16 pages, map only the top one
uint64 stack_bot = USER_STACK_TOP - STACK_MAX_PAGES * PG_SIZE;
struct vma *stack_vma = vma_create(stack_bot, USER_STACK_TOP,
                                   PTE_R | PTE_W | PTE_U, VMA_STACK);

// Only allocate + map the top page (where sp starts)
void *top_pg = kalloc();
map_pages(new_pt, USER_STACK_TOP - PG_SIZE, PG_SIZE, (uint64)top_pg,
          PTE_R | PTE_W | PTE_U);
```

When the stack grows past the first page (sp decreases below
`USER_STACK_TOP - PG_SIZE`), the process touches an address that has
no PTE but is covered by the stack VMA. The fault handler runs
`lazy_alloc` — same path as heap demand paging. No directional logic
needed: `vma_find` doesn't care whether memory grows up or down.

The hard limit is the VMA boundary. If the stack overflows all 16
pages and touches the guard page below `stack_bot`, there's no VMA →
`vma_find` returns NULL → the process is killed. Clean stack overflow
detection with zero runtime overhead.

---

## Part 5: Page Fault Dispatch

### How the hardware delivers page faults

When a RISC-V hart encounters a page-level exception, it:
1. Sets `scause` to one of:
   - 12 = instruction page fault (fetch from unmapped/non-executable)
   - 13 = load page fault (read from unmapped/non-readable)
   - 15 = store/AMO page fault (write to unmapped/non-writable)
2. Sets `stval` to the faulting virtual address
3. Traps to `stvec` → lands in our `user_trap`

### Routing in user_trap

Currently, the exception default case kills the process:

```c
default:
    kprintf("user_trap: exception ...");
    p->killed = 1;
    break;
```

We replace this with fault dispatch:

```c
case EXC_LOAD_PAGE_FAULT:    // 13
case EXC_STORE_PAGE_FAULT:   // 15
    if (handle_page_fault(p, code, csrr(stval)) < 0)
        p->killed = 1;
    break;
default:
    kprintf("user_trap: exception ...");
    p->killed = 1;
    break;
```

We handle load faults (13) and store faults (15). Instruction faults
(12) remain fatal — our text segments are eagerly mapped by exec, so
a fetch fault always means a genuine bug (jumping to unmapped memory).

### The unified fault handler

This is the decision tree — the architectural centerpiece of Round 6-4:

```c
int handle_page_fault(struct proc *p, uint64 cause, uint64 va) {
    struct vma *vma = vma_find(p, va);
    if (!vma)
        return -1;  // no VMA covers this address → segfault

    pte_t *pte = walk(p->pagetable, va, 0);

    // Case 1: no valid PTE → demand paging (lazy allocation)
    if (!pte || !(*pte & PTE_V)) {
        // A store to a non-writable region is still illegal
        if (cause == EXC_STORE_PAGE_FAULT && !(vma->perm & PTE_W))
            return -1;
        // No analogous check for loads: all our VMAs include PTE_R,
        // so a load to a VMA-covered address is always legitimate.
        return lazy_alloc(p, va, vma);
    }

    // Case 2: valid PTE + store fault + COW bit → copy-on-write
    if (cause == EXC_STORE_PAGE_FAULT && (*pte & PTE_COW)) {
        return cow_copy(p, va, pte);
    }

    // Anything else is a genuine access violation
    return -1;
}
```

The decision tree in diagram form:

```
handle_page_fault(va, cause):
│
├─ vma = vma_find(va)
│  └─ NULL? → return -1 (segfault — no region owns this address)
│
├─ pte = walk(va)
│  └─ missing or invalid?
│     ├─ store to non-writable VMA? → return -1 (real violation)
│     └─ else → lazy_alloc (demand page)
│
├─ store fault + PTE_COW set?
│  └─ yes → cow_copy (copy-on-write)
│
└─ else → return -1 (genuine violation)
```

> **Missing vs invalid PTE:** `walk()` returns NULL (missing) when an
> intermediate page table level was never allocated — the VA range has
> never been touched. This is the typical case for lazy heap/stack
> pages. A PTE is invalid (PTE_V cleared) when the entry exists in the
> page table but was zeroed — e.g., a page that was COW-shared and
> then freed by the other process via `page_put`, or an internal
> cleanup path. In practice, most faults that reach here are the
> "missing" case (fresh lazy pages). Both cases enter the same
> `lazy_alloc` path.

---

## Part 6: The Handlers

### lazy_alloc

```c
int lazy_alloc(struct proc *p, uint64 va, struct vma *vma) {
    void *pg = kalloc();   // returns zeroed page
    if (!pg)
        return -1;         // OOM → kill process

    uint64 aligned = PG_ROUND_DOWN(va);
    if (map_pages(p->pagetable, aligned, PG_SIZE, (uint64)pg, vma->perm) < 0) {
        kfree(pg);
        return -1;
    }
    return 0;
}
```

Simple: the VMA tells us the address is legitimate and what
permissions it should have. We just provide the physical page. The
page is already zeroed by `kalloc` — this is a security requirement,
not a convenience: without zeroing, a process could read stale data
from another process's freed pages.

> **When is memory zeroed?**
>
> | Source | Zeroed? | Reason |
> |--------|---------|--------|
> | Global/static variables | Yes | C standard guarantee (§6.7.9) |
> | Stack locals | No | Uninitialized — whatever was on the stack |
> | `malloc` return | No | May recycle memory from the free-list |
> | `calloc` return | Yes | Explicitly specified to zero (`malloc` + `memset(0)`) |
> | Kernel giving new pages (sbrk, mmap, page fault) | Yes | Security — can't leak other processes' data |
>
> The kernel always zeroes fresh pages. But user-space `malloc` may
> hand back a recycled chunk from its free-list without zeroing, so
> programs cannot assume `malloc`'d memory is zero.

### cow_copy

```c
int cow_copy(struct proc *p, uint64 va, pte_t *pte) {
    uint64 pa = pte_to_pa(*pte);
    struct page *pg = pa_to_page(pa);  // physical addr → page metadata (refcount lives here)

    if (pg->refcount == 1) {
        // Sole owner — just fix permissions, no copy needed
        *pte = (*pte & ~PTE_COW) | PTE_W;
        sfence_vma();
        return 0;
    }

    // Multiple owners — must copy
    void *new_pg = kalloc();
    if (!new_pg)
        return -1;  // OOM → kill process
    memcpy(new_pg, (void *)pa, PG_SIZE);

    // Remap to new page with full write permissions
    int flags = (pte_flags(*pte) & ~PTE_COW) | PTE_W;
    *pte = pa_to_pte((uint64)new_pg) | flags;
    sfence_vma();

    page_put((void *)pa);  // drop reference to old shared page
    return 0;
}
```

Two paths:
- **refcount == 1**: we're the sole owner (the other process already
  exec'd or exited). Just flip bits — zero allocation cost.
- **refcount > 1**: allocate, copy, remap, release old reference.
  The old page's refcount decrements; if the other process also
  COW-faults later, it will see refcount==1 and take the fast path.

### OOM policy

If `kalloc` fails during cow_copy or lazy_alloc, we return -1 and
the fault handler sets `p->killed = 1`. The process is terminated.

There's no graceful recovery — the process wrote to memory it
logically owns, and we can't provide backing storage. This matches
Linux's behavior for anonymous memory COW faults (the OOM killer
intervenes when physical memory is exhausted). In a system with swap,
the kernel would evict pages to disk first; we don't have swap, so
OOM is fatal.

---

## Part 7: Pre-faulting in copyout/copyin

### The problem

Consider this sequence:

```c
char *buf = sbrk(4096);    // heap grows, but page isn't allocated yet
read(fd, buf, 100);        // kernel tries to copyout into buf
```

The `read` syscall uses `copyout` to write data into user space.
copyout walks the page table to find the physical address behind
`buf` — but the page hasn't been faulted in (no PTE exists). copyout
would fail with -EFAULT, even though the address is perfectly valid.

### The solution: pre-fault in copyout

Before writing to a user page, copyout checks whether the PTE is
missing. If the VMA covers the address, it faults the page in
proactively:

```c
// Inside copyout, before memcpy:
pte_t *pte = walk(pagetable, va, 0);
if (!pte || !(*pte & PTE_V)) {
    struct vma *vma = vma_find(p, va);
    if (!vma || lazy_alloc(p, va, vma) < 0)
        return -EFAULT;
    pte = walk(pagetable, va, 0);  // re-walk after allocation
}
```

Similarly, if copyout writes to a COW page (store semantics), it
must trigger the COW copy:

```c
if (*pte & PTE_COW) {
    if (cow_copy(p, va, pte) < 0)
        return -EFAULT;
}
```

### Why not handle kernel-mode page faults instead?

An alternative design: let the kernel's memcpy trap on the unmapped
page, and handle the fault in `kernel_trap`. This is what Linux does
— it handles page faults from both user and kernel mode.

We chose pre-faulting because:

1. **Simpler invariant**: kernel page faults remain always-fatal
   (always a bug). No "is this an expected fault or a real bug?"
   analysis in kernel_trap.
2. **Localized change**: the fix lives entirely in copyout/copyin —
   two functions. A kernel-fault approach touches kernel_trap,
   needs to distinguish syscall-context from scheduler-context,
   and requires error propagation back to the caller.
3. **Our surface area is small**: the only kernel paths that touch
   user memory are copyout, copyin, and copyinstr. Three call sites
   to guard vs. a whole new fault handling path.

Linux needs kernel-fault handling because it has hundreds of paths
that access user memory (and `copy_to_user`/`copy_from_user` use
special exception tables to unwind on fault). We have three functions.
Pre-faulting is the proportionate solution.

---

## Part 8: Changes to Existing Code (Summary)

### New files
- `kernel/vm_fault.c` — `handle_page_fault`, `lazy_alloc`, `cow_copy`

### Modified files

| File | Change |
|------|--------|
| `kernel/include/vma.h` | Add `uint32 flags` to struct vma; define VMA_HEAP, VMA_STACK; add `flags` param to `vma_create`; declare `vma_find_by_flags` |
| `kernel/vma.c` | Implement `vma_find_by_flags`; copy flags in `vma_dup_all` |
| `kernel/include/vm.h` | Define `PTE_COW (1L << 8)` |
| `kernel/exec.c` | Compute heap_start after PT_LOAD, create zero-length heap VMA with VMA_HEAP flag; stack VMA spans STACK_MAX_PAGES but only top page mapped |
| `kernel/proc.c` (proc_fork) | Replace deep-copy loop with COW loop; flush TLB |
| `kernel/proc.c` (proc_sbrk) | New function: find heap VMA, grow/shrink, unmap on shrink |
| `kernel/syscall.c` | Add `sys_sbrk` (SYS_sbrk = 9) to dispatch table |
| `kernel/trap.c` (user_trap) | Route scause 13/15 to `handle_page_fault` |
| `kernel/vm.c` (copyout) | Pre-fault lazy pages and COW pages before memcpy |
| `kernel/include/riscv.h` or equivalent | Define `EXC_LOAD_PAGE_FAULT (13)`, `EXC_STORE_PAGE_FAULT (15)` |

---

## Quick Reference

### Syscall Table (updated)

| # | Name | Args | Returns |
|---|------|------|---------|
| 1 | write | fd, buf, len | bytes written |
| 2 | exit | status | (no return) |
| 3 | fork | — | child PID (parent) / 0 (child) |
| 4 | exec | path, argv | argc (no return on success) |
| 5 | wait | status_ptr | child PID |
| 6 | getpid | — | PID |
| 7 | kill | pid | 0 or -1 |
| 8 | sleep | ms | 0 |
| 9 | **sbrk** | **n (bytes)** | **old break, or -1** |

### Page Fault scause Codes

| Code | Name | Trigger |
|------|------|---------|
| 12 | Instruction page fault | Fetch from unmapped/non-exec page |
| 13 | Load page fault | Read from unmapped/non-readable page |
| 15 | Store/AMO page fault | Write to unmapped/non-writable page |

### PTE Bit Layout

| Bit | Name | Source | Meaning |
|-----|------|--------|---------|
| 0 | V | Hardware | Valid |
| 1 | R | Hardware | Readable |
| 2 | W | Hardware | Writable |
| 3 | X | Hardware | Executable |
| 4 | U | Hardware | User-accessible |
| 5 | G | Hardware | Global |
| 6 | A | Hardware | Accessed |
| 7 | D | Hardware | Dirty |
| 8 | **COW** | **Software** | **Copy-on-write marker** |
| 9 | — | Software | Reserved (free for future use) |

### VMA Flags

| Flag | Value | Meaning |
|------|-------|---------|
| VMA_HEAP | (1 << 0) | Heap region (growable via sbrk) |
| VMA_STACK | (1 << 1) | Stack region (elastic, demand-paged) |

### Fault Handler Decision Tree

```
handle_page_fault(va, cause):
│
├─ vma = vma_find(va)
│  └─ NULL → KILL (segfault)
│
├─ pte = walk(va)
│  └─ missing / invalid?
│     ├─ store to non-writable VMA → KILL
│     └─ else → lazy_alloc (zero-fill page)
│
├─ store fault + PTE_COW?
│  ├─ refcount == 1 → clear COW, set W (sole owner)
│  └─ refcount > 1 → alloc + memcpy + remap (actual copy)
│
└─ else → KILL (genuine violation)
```

### Constants

| Name | Value | Meaning |
|------|-------|---------|
| STACK_MAX_PAGES | 16 | Max stack size in pages (64 KB) |
| HEAP_MAX | USER_STACK_TOP - (STACK_MAX_PAGES+1)*PG_SIZE | Heap ceiling (1 guard page below stack) |
| PTE_COW | (1L << 8) | RSW bit 0: copy-on-write marker |

### Key Invariants

| Invariant | Enforced by |
|-----------|-------------|
| Heap VMA start never moves | sbrk checks `new_end >= vma->start` |
| Heap can't reach stack | sbrk checks `new_end <= HEAP_MAX` |
| Stack can't exceed 16 pages | VMA boundary; fault below → no VMA → kill |
| COW pages have PTE_W cleared | fork loop clears it |
| COW pages have refcount ≥ 1 | page_get on share, page_put on unmap |
| Lazy pages have no PTE | sbrk doesn't allocate; fork skips them |
| Kernel faults are always bugs | pre-fault in copyout, not kernel trap |
