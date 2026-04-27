# Lecture 3-2: Page Allocator

> **Where we are**
>
> Phase 3 Round 3-1 established the physical memory vocabulary. We know
> the QEMU `virt` machine has 128 MB of DRAM at `0x80000000`--
> `0x88000000`. We have named constants for the boundaries (`KERN_BASE`,
> `PHYS_STOP`), for the page size (`PG_SIZE = 4096`), and for alignment
> (`PG_ROUND_UP`, `PG_ROUND_DOWN`). The linker script exports
> `_kernel_end`, marking where the kernel image stops. Everything above
> that (rounded up to a page boundary) is free memory -- potentially
> tens of thousands of 4 KB pages sitting there, doing nothing, because
> we have no way to hand them out.
>
> This lecture builds the **page allocator** -- the lowest layer of
> kernel memory management. Two functions, `kalloc()` and `kfree()`,
> will let the rest of the kernel acquire and release physical pages.
>
> But before writing code, we face a design decision. xv6 uses a bare
> free list with no per-page metadata -- the simplest possible
> allocator. Production kernels (Linux, FreeBSD, Windows) maintain a
> metadata array with one entry per physical page, enabling features
> like copy-on-write fork, page reclamation, and shared memory. These
> are fundamentally different architectures, and switching between them
> later is painful. This lecture examines both approaches, explains why
> production kernels need per-page metadata, and makes a deliberate
> choice for bobchouOS: we build a `struct page` array from day one,
> sized for the one feature we're committed to -- **COW fork**.
>
> By the end of this lecture, you will understand:
>
> - Why the kernel needs a dynamic page allocator at all
> - How xv6's allocator works (free list, `struct run`, no metadata)
> - What production kernels do differently (`struct page` array) and
>   why -- the full `malloc(100)` path from user space to physical page
> - Which features require per-page metadata and which don't
> - Why bobchouOS chooses COW fork as the target feature, and how
>   that shapes the allocator design
> - The free list data structure and how it threads through free pages
> - `kinit()` / `free_range()` -- how the free list is built at boot
> - `kfree()` -- push a page onto the free list, fill with junk
> - `kalloc()` -- pop a page off the free list, zero it
> - Fill-on-free (junk pattern) and zero-on-alloc -- why both exist
> - Safety checks: alignment, address range validation
> - How to extend the design later (swap, shared mmap, compaction)
>
> **xv6 book coverage:** This lecture absorbs Chapter 3, sections 3.4
> (Physical memory allocation) and 3.5 (Code: Physical memory
> allocator). We also touch on the "Real world" section (3.9) as it
> relates to allocator design.

---

## Part 1: Why a Page Allocator?

### The problem

So far, every data structure in our kernel has a size known at compile
time. The `ticks` counter is a global variable in `.bss`. The
`timer_scratch` array is a fixed-size buffer. The `exc_names` table is
a constant array. Even the kernel stack is a fixed 16 KB block
allocated in the linker script.

This works when you know exactly what the kernel needs before it runs.
But that assumption breaks the moment you want to do anything dynamic:

- **Page tables.** When we build the kernel's page table in Phase 4,
  we need pages to hold the page table entries. How many? It depends
  on how much virtual address space we map, which depends on how much
  physical memory is available. We can't know this at compile time.

- **Process stacks.** Each process needs its own kernel stack. How
  many processes will run? We don't know until runtime.

- **User memory.** When a process calls `sbrk()` to grow its heap,
  the kernel needs to allocate a new physical page and map it into the
  process's address space. This happens at arbitrary times during
  execution.

- **Pipe buffers, inodes, network buffers...** Every kernel
  subsystem eventually needs memory that can't be statically
  pre-allocated.

The kernel needs a way to say "give me a page of physical memory" and
"I'm done with this page." That's `kalloc()` and `kfree()`.

### Why pages, not bytes?

You might wonder: why not a general-purpose allocator like `malloc()`
that can hand out any number of bytes? Two reasons:

**1. The hardware works in pages.** The page table maps virtual
addresses to physical addresses in 4096-byte units. When the kernel
maps memory for a process, it must provide page-aligned physical
addresses. A byte-granularity allocator would need to round up to
pages anyway for anything that touches the page table.

**2. Simplicity.** A page allocator has exactly one "size class" --
every allocation is 4096 bytes, every free is 4096 bytes. No
fragmentation decisions, no splitting, no coalescing, no size headers.

xv6 has no `malloc()` at all. Every kernel data structure is either
statically sized (fixed-size arrays) or built from whole pages. This
is an intentional simplification -- a real kernel (Linux, FreeBSD)
has both a page allocator *and* a slab/SLUB allocator layered on top
for small objects. But the page allocator is always the foundation.

> **Where does `malloc()` fit in a real kernel?**
>
> Linux has a layered memory allocation architecture:
>
> ```
> Layer 4:  kmalloc() / kfree()        <-- arbitrary-sized allocations
>             |                              (8 bytes to 8 KB typical)
> Layer 3:  SLUB allocator              <-- pools of same-sized objects
>             |                              ("slab caches")
> Layer 2:  Buddy allocator             <-- allocates runs of 2^n pages
>             |                              (1, 2, 4, 8, ... pages)
> Layer 1:  struct page array           <-- metadata for every physical
>                                            page in the system
> ```
>
> **Layer 1** is the foundation: at boot, Linux creates an array of
> `struct page` descriptors -- one for every physical page of RAM.
> This array *is* the bookkeeping -- the source of truth for every
> page's state. Given a physical address, you can look up its
> `struct page` in O(1) by array index. On a 64 GB server, that's
> ~16 million entries (64 bytes each = ~1 GB just for page metadata --
> about 1/64 of RAM, which is a tolerable ~1.5% overhead).
>
> **Layer 2** is the **buddy allocator**, which manages all physical
> pages. It maintains 11 free lists (each a doubly-linked list so
> blocks can be removed from the middle in O(1) during merging): one
> for single pages (order 0), one for pairs of pages (order 1), one
> for groups of 4 (order 2), up to groups of 1024 pages (order 10 =
> 4 MB). When you ask for a page, it pops from the order-0 free list.
> When the order-0 list is empty (no free single pages available), it
> splits a pair from order 1 into two singles, serves one, and puts
> the other on the order-0 list. When you free two adjacent pages that
> were originally a pair ("buddies"), they merge back into the order-1
> list. This splitting/coalescing prevents fragmentation while keeping
> allocation fast (mostly O(1)).
>
> **How Layers 1 and 2 work together:** the `struct page` array and
> the free lists are two data structures serving different purposes.
> The array is the source of truth -- it tells you everything about
> any page (free? refcount? order?). The free lists are an
> acceleration structure -- without them, finding a free page would
> require scanning the entire array, which is O(n). With them, it's
> O(1): just pop the head. The free lists *can't* work alone either:
> when merging buddies, the allocator needs to check "is my buddy
> free?" -- it finds the buddy by address arithmetic
> (`buddy = addr XOR (1 << (order + 12))`), then checks the buddy's
> `struct page` entry. The array answers the question, the free list
> avoids the scan. Think of it like a database table (the array) with
> an index (the free lists).
>
> **Layer 3** is the **SLUB allocator** (Linux's current default; SLAB
> and SLOB are older alternatives). It creates "slab caches" -- pools
> of identically-sized objects. For example, there's a cache for
> `struct inode` (one per open file), a cache for `struct task_struct`
> (one per process), and general-purpose caches for power-of-2 sizes
> (32, 64, 128, 256, ... bytes). Each cache gets pages from the buddy
> allocator and subdivides them into fixed-size slots. When you free
> an object, its slot goes back to the cache, not to the buddy system.
> This avoids constantly requesting and releasing pages for common
> allocations.
>
> **Layer 4** is `kmalloc()` -- the kernel's "give me N bytes" API.
> It finds the right general-purpose slab cache (the smallest
> power-of-2 size >= N) and allocates a slot from it. So
> `kmalloc(100)` goes to the 128-byte cache, `kmalloc(500)` goes to
> the 512-byte cache.
>
> xv6 has only the equivalent of Layer 2 -- a page allocator -- but
> a much simpler one: a flat singly-linked list, no buddy splitting,
> no Layer 1 metadata. bobchouOS will add a minimal Layer 1 on top of
> xv6's approach. More on this in Part 3.

---

## Part 2: How xv6 Does It

Before we discuss design choices, let's understand xv6's allocator --
the baseline we're building from.

### The free list

xv6 uses the simplest possible allocator for fixed-size blocks: a
singly-linked list threaded through the free pages themselves.
Each free page contains a pointer to the next free page. The allocator
maintains a `freelist` pointer to the head. Allocating pops the head.
Freeing pushes onto the head. Both operations are O(1).

```
freelist
   |
   v
+--------+     +--------+     +--------+     +--------+
| next --|---->| next --|---->| next --|---->| NULL   |
| (rest  |     | (rest  |     | (rest  |     | (rest  |
|  of    |     |  of    |     |  of    |     |  of    |
|  page  |     |  page  |     |  page  |     |  page  |
|  is    |     |  is    |     |  is    |     |  is    |
| unused)|     | unused)|     | unused)|     | unused)|
+--------+     +--------+     +--------+     +--------+
  4096 B         4096 B         4096 B         4096 B
```

### Metadata inside the free pages

A free page has 4096 bytes of space that nobody is using. xv6 stores
the linked list node *inside* the page itself -- it only needs
8 bytes (one pointer) out of 4096. When the page gets allocated and
returned to the caller, the caller overwrites those bytes with their
own data. The list node is gone, and that's fine, because the page is
no longer on the free list.

This "embed metadata in the free block" pattern is universal. It's
used in:

- xv6's `kalloc.c` (exactly what we're building)
- Linux's buddy allocator (the first word of a free page links buddy
  pairs)
- `malloc()` implementations like glibc's ptmalloc, dlmalloc, jemalloc
  (free chunks contain forward/backward pointers)
- Slab allocators (free objects in a slab contain a next-free pointer)

### `struct run`

xv6 defines the list node as:

```c
// kernel/kalloc.c (xv6)
struct run {
    struct run *next;
};
```

That's the entire structure -- one pointer. To turn a free page into a
list node, you cast the page's address to `struct run *`:

```c
struct run *r = (struct run *)page_address;
r->next = freelist;
freelist = r;
```

The page's first 8 bytes now hold the `next` pointer. The remaining
4088 bytes are don't-care (they'll be filled with junk by `kfree()`).

> **Why a struct instead of a raw pointer?**
>
> We could use `void **` or `uint64 *` instead of `struct run`. The
> struct buys us:
>
> 1. **Type safety.** `struct run *` is a distinct type from other
>    pointers. If you accidentally pass a `struct run *` where a
>    `struct page_table *` is expected, the compiler warns you. With
>    `void **`, everything is compatible with everything.
>
> 2. **Self-documenting code.** `r->next` is clearer than `*(void **)r`.
>    The struct name tells you "this is a linked list node for the free
>    list," not "this is some random pointer I'm dereferencing."
>
> 3. **Extensibility.** If we later wanted to add metadata to free
>    pages (a generation counter for debugging, a magic value for
>    validation), we add a field to the struct. With raw pointers,
>    we'd need to manually compute offsets.
>
> The compiled code is identical either way -- the struct has no
> overhead beyond what a raw pointer would have.

### LIFO order (stack behavior)

`kfree()` pushes onto the head (prepend). `kalloc()` pops from the
head. This makes the free list a LIFO stack -- the most recently freed
page is the first to be reallocated.

Why LIFO? Because it's the simplest O(1) implementation (no need to
traverse the list or maintain a tail pointer). It also has a nice
cache behavior property: recently freed pages are more likely to still
be in the CPU cache, so reallocating them may be faster than
allocating a page that hasn't been touched in a while.

The order of pages in the free list has no effect on correctness. Any
page is as good as any other for any purpose. The caller doesn't care
*which* page it gets, just that it gets one.

### xv6's `kalloc.c` -- the key parts

xv6's allocator is ~80 lines total. Here's the structure, stripped
to the essentials:

```c
// kernel/kalloc.c (xv6-riscv), simplified

extern char end[];              // linker symbol: first address after kernel

struct run { struct run *next; };

struct run *freelist;           // head of free list (actually inside kmem struct with a lock)

void kinit()           { freerange(end, (void*)PHYSTOP); }

void freerange(void *start, void *end) {
    for (char *p = (char*)PGROUNDUP((uint64)start); p + PGSIZE <= (char*)end; p += PGSIZE)
        kfree(p);
}

void kfree(void *pa) {
    if (/* not aligned || below kernel || above PHYSTOP */) panic("kfree");
    memset(pa, 1, PGSIZE);             // junk fill
    struct run *r = (struct run*)pa;
    r->next = freelist;                 // prepend to free list
    freelist = r;
}

void *kalloc(void) {
    struct run *r = freelist;
    if (r) freelist = r->next;          // pop from free list
    if (r) memset((char*)r, 5, PGSIZE); // junk fill (NOT zero)
    return (void *)r;
}
```

Key things to notice:

- **`struct run` is the only metadata.** It exists inside free pages
  and vanishes on allocation. Once a page is allocated, xv6 has
  *zero* information about it -- no refcount, no owner, no status.

- **Spinlock.** xv6 protects the free list with `kmem.lock` because
  multiple harts may call `kalloc()`/`kfree()` concurrently. We run
  on one hart, so we'll skip the lock.

- **`kalloc()` fills with junk, not zero.** xv6 fills allocated pages
  with the byte `0x05` and relies on callers to zero the page if they
  need it clean. We'll change this (discussed in Part 6).

- **`kfree()` validation.** Three checks: page-aligned, not inside
  the kernel image, not beyond PHYSTOP. Any failure panics.

### The limits of xv6's approach

xv6's allocator is elegant and correct. But the "zero metadata on
allocated pages" design has a hard ceiling. xv6 can't answer:

- "How many processes share this page?" (no refcount)
- "Which page should I evict to disk?" (no LRU tracking)
- "Who maps this page?" (no reverse mapping)

xv6 doesn't need to answer these because it doesn't implement the
features that ask them. But any OS that does needs per-page metadata.

---

## Part 3: The Design Decision -- Per-Page Metadata

### What production kernels know about each page

Linux's `struct page` is ~64 bytes and tracks everything the kernel
might need to know about a physical page. Here's a simplified view of
the fields that matter for our discussion:

```c
// Simplified from Linux's include/linux/mm_types.h
struct page {
    unsigned long  flags;       // PG_locked, PG_dirty, PG_lru, ...
    int            _refcount;   // how many users (PTEs, kernel refs)
    int            _mapcount;   // how many page table mappings
    struct list_head lru;       // position on active/inactive LRU list
    struct address_space *mapping;  // who owns this page
    pgoff_t        index;       // offset within the owner
    // ... many more fields (compound pages, slab, swap, etc.)
};
```

FreeBSD has `vm_page`, Windows has the PFN database -- different
names, same idea: one metadata entry per physical page, stored in a
flat array indexed by physical page number.

### Which features need per-page metadata?

Not every kernel feature requires this. Here's the breakdown, ranked
by how essential they are:

| Rank | Feature | Metadata needed | What it enables |
|------|---------|----------------|-----------------|
| 1 | **COW fork** | `refcount` | `fork()` shares pages read-only between parent and child. On write, the kernel checks refcount > 1 → copy the page, give the writer a private copy, decrement refcount on the original. Without COW, `fork()` eagerly copies every page -- unusably slow for any non-trivial process. |
| 2 | **Swap / page reclaim** | `refcount`, LRU lists, dirty bit | When physical memory is full, evict cold pages to disk. Requires knowing which pages are least recently used (LRU) and whether they've been modified (dirty). Without swap, `kalloc()` returns NULL and you're dead. |
| 3 | **Shared mmap** | `refcount` | `mmap` is how modern programs load shared libraries (libc, libm). Every process maps the same `.so` file -- without sharing, each gets its own copy in RAM. Also needed for shared memory IPC. |
| 4 | **Reverse mapping** | `mapping`, `index` | Given a physical page, find all PTEs that map it. Required for correct swap-out of shared pages (must invalidate every PTE pointing to the evicted page). |
| 5 | **Memory compaction** | migration type | Move pages around to create large contiguous free regions (for huge pages, DMA buffers). Only matters for long-running servers under fragmentation pressure. |

Features #2, #3, and #4 come as a package in practice -- swap needs
reverse mapping to invalidate PTEs of shared pages being evicted, and
shared mmap needs refcounting to know when the last mapper releases a
page. You don't implement one without the others.

**COW fork is the exception.** It only needs a refcount. No LRU, no
reverse mapping, no swap. It's also the single feature whose absence
is most painful -- every Unix program that spawns a child (`sh`,
`make`, `gcc`) relies on `fork()` + `exec()` being fast. xv6's eager
copy makes `fork()` O(memory size) instead of O(page table size).

### What xv6 skips

xv6 doesn't implement *any* of these features:

- **`fork()`** copies every page eagerly (`uvmcopy()` calls `kalloc()`
  and `memmove()` for each page). No sharing, no refcount, no COW.
- **No swap.** `kalloc()` returns 0 and that's it.
- **No shared mmap.** No `mmap` at all.
- **No reverse mapping.** When a process exits, xv6 walks its page
  table to find pages to free -- that's virtual→physical direction.
  It never needs physical→virtual.

This works for a teaching OS with tiny processes and 128 MB of RAM.
But it means xv6's allocator architecture can't grow to support these
features without being restructured.

### The full picture: `malloc(100)` twice

To make this concrete, let's trace what happens in both Linux and xv6
when a user process calls `malloc(100)` twice.

**Linux:**

1. First `malloc(100)`. glibc's allocator has no memory, so it calls
   `brk()` to extend the heap. The kernel extends the process's VMA
   but with **lazy allocation** -- no physical page yet, just a note
   that the address range is valid.

2. User writes to the returned pointer. CPU walks the page table --
   no PTE exists. **Page fault.** Kernel's fault handler calls the
   buddy allocator to get a physical page, **updates `struct page`**
   (sets refcount to 1, flags, LRU list position), zeroes the page,
   creates a PTE, returns.

3. Second `malloc(100)`. glibc carves another chunk from the same
   arena. 200 bytes total, still within one 4 KB page. **No syscall,
   no page fault, no kernel involvement.**

**xv6:**

1. First `malloc(100)`. xv6's minimal `umalloc.c` calls `sbrk(4096)`.
   Kernel's `sys_sbrk()` → `growproc()` → `uvmalloc()` calls
   `kalloc()` -- pops the head of the free list, zeroes the page,
   maps it immediately (no lazy allocation). Returns to user.

2. xv6's malloc carves a chunk from the page. Returns pointer.

3. Second `malloc(100)` -- same as Linux: carved from the existing
   page in user space. No kernel involvement.

Summary: both systems allocate one physical page for two
`malloc(100)` calls. The first triggers the full kernel path; the
second is pure user-space bookkeeping. The difference is what happens
*around* that kernel allocation:

| Step | Linux | xv6 |
|------|-------|-----|
| User allocator | glibc ptmalloc (bins, arenas, thread caches) | `umalloc.c` (~50 lines, simple free list) |
| Syscall to grow heap | `brk()` -- extends VMA, no physical alloc | `sbrk()` -- allocates physical page immediately |
| Page allocation | Buddy allocator → updates `struct page` (refcount, flags, LRU) | Pop from free list. Nothing else. |
| When is the page mapped? | Lazily, on first access (page fault) | Immediately in `sbrk()` |
| Per-page metadata | `struct page`: refcount=1, on LRU list, flags set | None. The page exists; nobody tracks it. |

For basic allocation, the metadata overhead buys nothing visible. The
payoff comes from all the features in the table above: `fork()` can
share pages instead of copying (refcount 1 → 2), the kernel can evict
cold pages to disk under memory pressure (LRU lists), shared libraries
are mapped once instead of duplicated per process (refcount). Without
per-page metadata, none of these are possible.

### bobchouOS's choice

bobchouOS is an educational OS, but its fundamental architecture
should not diverge from production kernels where the divergence is a
one-way door. Switching from "no metadata" to "per-page metadata
array" later would require touching every allocation path.

Our choice: **build a `struct page` array from day one**, sized for
COW fork. The struct is minimal:

```c
struct page {
    uint16 refcount;     // number of PTEs mapping this page
};
```

That's 2 bytes per page. For our 128 MB of DRAM (32,768 pages), the
array costs **64 KB** -- 0.05% of RAM. Negligible.

The refcount starts at 0 (free). `kalloc()` sets it to 1. `kfree()`
requires it to be 1 (or panics -- someone is freeing a shared page
without decrementing first). COW fork will increment it when sharing
a page between parent and child. The COW fault handler will decrement
it when making a private copy.

For now (Phase 3), the refcount field is present but only does
basic alloc/free bookkeeping. Its real purpose -- shared page
tracking -- comes alive when we implement COW fork.

> **Why `uint16` and not `uint32` or `int`?**
>
> A page's refcount is bounded by the number of processes that can
> map it. With 64 processes (xv6's `NPROC`), a page can have at most
> 64 references. `uint16` holds up to 65,535 -- far more than enough.
> Using a smaller type keeps the struct compact (2 bytes vs. 4),
> which matters when you have 32,768 entries.
>
> **Why does xv6 have a 64-process limit?** Because xv6 uses a
> fixed-size array: `struct proc proc[NPROC]` with `NPROC = 64`,
> statically allocated in `.bss`. No `kmalloc()` means no dynamic
> resizing -- when all 64 slots are in use, `fork()` simply fails.
> This is the direct consequence of having no small-object allocator:
> every kernel data structure must be a fixed-size array with a
> compile-time limit.
>
> **Linux has no hard-coded limit.** Process descriptors
> (`struct task_struct`, ~6 KB each) are allocated dynamically from a
> slab cache. The practical limits are:
>
> - **PID range:** default max is 32,768, configurable up to
>   4,194,304 via `/proc/sys/kernel/pid_max`
> - **Memory:** each process costs ~10-20 KB of kernel memory
>   (task struct + kernel stack + page tables). A 64 GB server could
>   theoretically hold millions.
> - **Tunable cap:** `/proc/sys/kernel/threads-max`, typically set to
>   a fraction of available RAM at boot
>
> In practice, a typical Linux server runs hundreds to low thousands
> of processes. Hitting the limit is rare outside of fork-bomb
> scenarios.
>
> Linux uses `atomic_t` (32-bit atomic integer) for the refcount
> because it needs atomicity for multi-core access. We run on one
> hart for now, so a plain `uint16` is fine. If we add multi-hart
> support later, this would need to become an atomic type or be
> protected by a lock.

### How to extend later

The `struct page` array is the foundation for future features. Each
requires adding fields:

| Feature | Add to `struct page` | When |
|---------|---------------------|------|
| **COW fork** | `uint16 refcount` (already present) | Phase 6 |
| **Swap** | `uint8 flags` (dirty, on-LRU), LRU list pointers | If we ever implement swap |
| **Reverse mapping** | Owner pointer, offset within owner | If we need shared page eviction |
| **Compaction** | Migration type field | Unlikely for bobchouOS |

Each extension is additive -- add a field to the struct, add code that
reads/writes it. The flat array lookup (`pages[(pa - KERN_BASE) / PG_SIZE]`)
never changes. By starting with the array now, we avoid a
restructuring later.

---

## Part 4: Initialization -- `kinit()` and `free_range()`

### The boot-time setup

At boot, the free list is empty and the `struct page` array doesn't
exist yet. `kinit()` must:

1. Carve the `struct page` array out of the free region (before the
   allocator is running -- we can't `kalloc()` the array because the
   allocator isn't initialized yet)
2. Walk the remaining free pages and add each one to the free list
   by calling `kfree()`

### xv6's `kinit()` and `freerange()`

```c
// kernel/kalloc.c (xv6), simplified
void kinit() {
    freerange(end, (void*)PHYSTOP);  // (also inits a spinlock, omitted)
}

void freerange(void *start, void *end) {
    for (char *p = (char*)PGROUNDUP((uint64)start); p + PGSIZE <= (char*)end; p += PGSIZE)
        kfree(p);
}
```

xv6's `kinit()` calls `freerange()` with the linker symbol `end`
(xv6's equivalent of our `_kernel_end`) as the start of free memory.
Let's trace through `freerange()`:

1. **Round up the start.** `PGROUNDUP((uint64)start)` aligns the
   start address to the next page boundary. If `end` is at
   `0x8000_A234`, this becomes `0x8000_B000`. The partial page between
   the kernel and this boundary is wasted (at most 4095 bytes).

2. **Loop through pages.** The `for` loop walks page by page,
   incrementing `p` by `PGSIZE` (4096) each iteration. The condition
   `p + PGSIZE <= (char*)end` ensures we don't create a page that
   extends past the end of DRAM.

3. **Free each page.** Each iteration calls `kfree(p)`, which pushes
   the page onto the free list.

> **Why does `freerange()` call `kfree()` instead of directly building
> the list?**
>
> Because `kfree()` does two important things beyond list insertion:
>
> 1. It fills the page with junk (the "fill-on-free" pattern --
>    described in Part 5).
> 2. It validates the address (checking alignment and range).
>
> Reusing `kfree()` avoids duplicating this logic. The "free list
> starts empty, fill it by kfree-ing everything" approach means the
> initialization code and the runtime free code share the same path.
> Any bug in `kfree()` shows up immediately at boot when thousands of
> pages are freed.
>
> This also means `kinit()` is *slow* -- it memsets every free page at
> boot. For 128 MB of DRAM minus the kernel, that's roughly 32,000
> pages * 4096 bytes = ~128 MB of writes. On a real machine this takes
> a measurable fraction of a second. xv6 doesn't care (it's a teaching
> OS), and neither do we. A production kernel would defer the zeroing
> (zero pages on demand when allocated, or use a background thread).

### Our `kinit()` -- with `struct page` array bootstrapping

Our `kinit()` is slightly more complex than xv6's because it must
bootstrap the `struct page` array before the allocator is running.
We skip the spinlock (single hart).

```
Memory layout during kinit():

0x80000000  +-------------------+ <-- KERN_BASE / _kernel_start
            |  Kernel image     |
            +-------------------+ <-- _kernel_end
            |  (round up)       |
            +-------------------+ <-- PG_ROUND_UP(_kernel_end)
            |  struct page[N]   |     carved out first, before allocator starts
            +-------------------+ <-- free_start (page-aligned)
            |                   |
            |  Free pages       |     added to free list by free_range()
            |                   |
            +-------------------+ <-- PHYS_STOP (0x88000000)
```

The sequence:

1. Compute `nr_pages` -- total number of physical pages from
   `KERN_BASE` to `PHYS_STOP`.
2. Place the `struct page` array at `PG_ROUND_UP(_kernel_end)`.
   Size: `nr_pages * sizeof(struct page)`.
3. Zero the array (all refcounts start at 0 = free).
4. Round up to the next page boundary -- this is where free pages
   start.
5. Call `free_range()` from that point to `PHYS_STOP`.

The kernel and the `struct page` array are "reserved" -- they never
enter the free list. Pages below `_kernel_end` have refcount 0 but
are never freed (they contain the kernel). The array itself sits in
the gap between the kernel and the free region.

> **xv6 book note:** xv6's book says "the allocator starts with no
> memory; these calls to `kfree` give it some to manage." This is a
> clean way to think about it: `kfree()` is not just "return a page
> you allocated" -- it's "add a page to the pool." At boot time, we're
> adding the *initial* pool. At runtime, we're returning pages that
> were previously allocated.

### How many pages?

A quick calculation for our QEMU setup:

- DRAM: 128 MB = 134,217,728 bytes
- Total pages: 134,217,728 / 4096 = 32,768 pages
- `struct page` array: 32,768 * 2 bytes = 65,536 bytes = 16 pages
- Kernel image: ~30-50 KB ≈ 8-13 pages
- Free pages: ~32,768 - 13 - 16 = ~32,739 pages

The array costs 16 pages out of 32,768 -- less than 0.05%. We'll
print the exact counts at boot.

### The free list after `kinit()`

Since `free_range()` walks from low addresses to high and `kfree()`
prepends to the list, the free list ends up in *reverse* order: the
highest-addressed page is at the head, and the lowest is at the tail.

```
After kinit():

free_list -> page at 0x87FFF000 (highest free page)
               -> page at 0x87FFE000
                    -> page at 0x87FFD000
                         -> ...
                              -> first free page above struct page array
                                   -> NULL
```

This means the first `kalloc()` call returns the page at `0x87FFF000`
(the highest address). Subsequent calls walk downward. This order is
arbitrary and has no effect on correctness.

---

## Part 5: `kfree()` -- Returning a Page

### What `kfree()` does

Given a pointer to a page, `kfree()` validates the address, fills the
page with junk (to catch dangling pointer bugs), sets the page's
refcount back to 0, and pushes it onto the free list.

### xv6's `kfree()` (simplified)

```c
// kernel/kalloc.c (xv6), lock removed
void kfree(void *pa) {
    if (/* not aligned || below kernel || above PHYSTOP */) panic("kfree");
    memset(pa, 1, PGSIZE);             // junk fill
    struct run *r = (struct run*)pa;
    r->next = freelist;                 // prepend to free list
    freelist = r;
}
```

Three steps: validate, junk-fill, prepend. Let's look at each.

### Validation

```c
if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
```

Three checks, any of which triggers a panic:

| Check | What it catches |
|-------|----------------|
| `pa % PGSIZE != 0` | Non-page-aligned address. Someone passed an address that's not the start of a page. This is always a bug -- `kalloc()` only returns page-aligned addresses. |
| `pa < end` | Address inside (or before) the kernel image. The kernel's own `.text`, `.data`, `.bss`, and stack must never be freed. |
| `pa >= PHYSTOP` | Address beyond the end of RAM. There's no physical memory there to free. |

These are "should never happen" checks that catch programming errors
early. A kernel that silently corrupts its own free list (by putting
an invalid page on it) would crash later in a way that's nearly
impossible to debug. Panicking immediately tells you *exactly* where
the bug is.

### Fill with junk (the "fill-on-free" pattern)

```c
memset(pa, 1, PGSIZE);
```

This fills the entire 4096-byte page with the byte `0x01`. Why? To
**catch dangling pointer bugs.** A dangling pointer is a pointer that
still references memory after it's been freed. Consider:

```c
struct thing {
    int type;
    int value;
    struct thing *next;
};

struct thing *t = (struct thing *)kalloc();
t->type = 42;
t->value = 100;
t->next = some_other_thing;

kfree(t);       // Page is freed

// BUG: code still uses 't' after freeing it
if (t->type == 42) {   // Reads freed memory!
    // Without junk fill: this reads 42 -- the old value is still
    // there. The code "works" but is dangling into freed memory.
    // The bug is silent and may only manifest when the page is
    // reallocated and overwritten by something else -- possibly
    // hours or days later.

    // With junk fill: this reads 0x01010101 = 16843009.
    // The value is unexpected, and the code probably breaks
    // immediately and visibly. The bug is caught quickly.
}

// Even worse with pointers:
t->next->value;  // Without junk: follows the old valid pointer.
                  // With junk: follows 0x0101010101010101,
                  //   which is not a valid kernel address.
                  //   This causes an access fault -- immediate crash.
```

The junk fill makes dangling pointer bugs **fail fast** -- they
corrupt visible state or trigger a crash quickly, rather than
silently working with stale data until something subtly breaks.

> **Why the value 1 and not 0 or 0xDEADBEEF?**
>
> The xv6 book says xv6 fills with 1. The choice is somewhat
> arbitrary, but there are reasons behind it:
>
> - **Not 0** -- because zero-filled memory looks like valid
>   initialized data. A zeroed pointer is NULL (which many programs
>   check for). A zeroed integer is a valid value. Zero is the least
>   "suspicious" value, so bugs involving freed memory would be
>   hardest to notice.
>
> - **Not 0xFF** (all-ones) -- because `0xFFFFFFFFFFFFFFFF` as a
>   pointer is `-1` in signed arithmetic, which some code checks as
>   an error sentinel. It's also the top of the address space, which
>   might coincidentally be mapped on some platforms.
>
> - **1 works well** -- `0x01` as a byte gives `0x0101010101010101`
>   as a 64-bit value. This is not a valid kernel pointer (it's way
>   below KERN_BASE), not NULL, not -1, and not a plausible integer
>   value for most kernel data structures. It's conspicuous.
>
> - **Linux uses different patterns.** Linux's SLUB allocator uses
>   `0x5a` for freed objects, `0x6b` for "red zones" (guard regions
>   around allocations to detect overflows), and `0xa5` for poisoned
>   memory. The different values let you diagnose *what kind* of
>   memory corruption occurred just by looking at the pattern in a
>   crash dump. This is called "slab poisoning" and is controlled by
>   `CONFIG_SLUB_DEBUG`.
>
> Production kernels can also use the fill pattern as a basic security
> measure -- clearing sensitive data (passwords, crypto keys) from
> freed memory prevents a later allocation from reading stale secrets.
> This is defense in depth, not a primary security boundary.

### Prepend to the free list

```c
r = (struct run*)pa;
r->next = kmem.freelist;
kmem.freelist = r;
```

This is a textbook singly-linked-list prepend:

1. Cast the page address to `struct run *`. Now `r` points to the
   first 8 bytes of the page (which were just filled with junk, and
   will be overwritten by the next pointer).

2. Set `r->next` to the current head of the free list. The new page's
   `next` pointer now points to what was previously the first free page.

3. Update the head to point to the new page.

```
Before kfree(page_X):

freelist -> page_A -> page_B -> page_C -> NULL

After kfree(page_X):

freelist -> page_X -> page_A -> page_B -> page_C -> NULL
```

> **Note about the junk fill and the next pointer.** The `memset` fills
> the entire page (including the first 8 bytes) with `0x01`. Then we
> immediately overwrite the first 8 bytes with `r->next = freelist`.
> So the junk in the first 8 bytes is short-lived -- it gets replaced
> by a valid pointer. The remaining 4088 bytes stay as junk until the
> page is allocated and zeroed by `kalloc()`.

---

## Part 6: `kalloc()` -- Allocating a Page

### What `kalloc()` does

`kalloc()` removes a page from the front of the free list, zeros it,
and returns its address. If the free list is empty, it returns NULL.

### xv6's `kalloc()`

```c
// kernel/kalloc.c (xv6), simplified
void *kalloc(void) {
    struct run *r = freelist;
    if (r) freelist = r->next;          // pop from free list
    if (r) memset((char*)r, 5, PGSIZE); // junk fill (NOT zero)
    return (void *)r;
}
```

xv6's `kalloc()` fills the allocated page with the byte `0x05` -- a
**junk pattern**, not zeros. The xv6 book explains: *"this will cause
code that reads garbage to read garbage instead of the old valid
contents."* The philosophy is the same as `kfree()`'s fill with 1:
make stale data obviously wrong.

xv6 then relies on *callers* to zero the page if they need it clean.
For example, `uvmalloc()` (which allocates user memory pages) calls
`memset(mem, 0, PGSIZE)` after `kalloc()`.

### xv6's choice vs. ours

This is a valid design choice, but it means every caller that needs a
zeroed page has to remember to zero it. Forget once, and you get a
page full of `0x05` interpreted as user data or page table entries.

**Our choice: zero-on-alloc.** We'll have `kalloc()` return a
pre-zeroed page. This is safer:

- Page tables *require* zeroed pages (an uninitialized PTE is
  garbage, not "unmapped"). With zero-on-alloc, new page table pages
  are automatically clean.
- User memory should be zeroed (a process shouldn't see stale kernel
  data from a previous process -- that's an information leak).
- The caller can always overwrite the zeros if they don't need them.
  Zeroing an already-zeroed page is wasteful but harmless. Forgetting
  to zero a junk-filled page is a bug.

The cost is one extra `memset` per allocation (zeroing 4096 bytes).
At our scale this is negligible.

> **A note on double-touching.** With our approach, every page gets
> written twice on the free/alloc cycle: once with junk (by `kfree()`)
> and once with zeros (by `kalloc()`). This is 8 KB of writes per
> 4 KB page cycle. In a teaching OS, this is fine. In a production
> kernel, you'd optimize:
>
> - **Linux** zeroes pages in a background thread (`kzerod` on some
>   configs) or uses hardware-assisted zeroing. It does *not* junk-fill
>   on every free -- that's only done in debug builds
>   (`CONFIG_SLUB_DEBUG`).
> - **FreeBSD** maintains a separate "zero page" list. When the system
>   is idle, a daemon zeroes free pages preemptively. `kalloc` for a
>   page table can grab a pre-zeroed page without any `memset`.
> - Some ARM and x86 CPUs have `DC ZVA` / `REP STOSB` instructions
>   that zero a cache line without first reading it from memory
>   (avoiding the read-for-ownership bus transaction), making zeroing
>   nearly twice as fast.

---

## Part 7: How Real Allocators Differ

### The buddy system

xv6's flat linked list (and ours) has a limitation: every allocation
is exactly one page. What if you need 8 contiguous pages for a large
DMA buffer? You'd have to call `kalloc()` eight times and *hope* the
pages happen to be contiguous. They probably won't be.

Linux's **buddy allocator** solves this. It maintains 11 free lists,
one for each "order" (power of 2):

```
Order 0:  single pages (4 KB)      [most common]
Order 1:  pairs of pages (8 KB)
Order 2:  groups of 4 (16 KB)
Order 3:  groups of 8 (32 KB)
  ...
Order 10: groups of 1024 (4 MB)    [rare]
```

Allocation: to get 2^k pages, take a block from order k's free list.
If order k's list is empty, take a block from order k+1 and split it
into two "buddies" — serve one, put the other on order k's list.

Freeing: put the block on order k's list. Check if its buddy (the
other half of the original pair) is also free. If so, merge them and
promote to order k+1. Repeat upward.

The name "buddy" comes from this pairing: every block has a buddy at a
known address (flip bit k+12 of the address). This makes the merge
check O(1).

> **Example: allocating and freeing in a buddy system**
>
> Say we have 16 pages of memory (orders 0-4):
>
> ```
> Initial state:
>   Order 4: [page 0-15]  (one block of 16 pages)
>   All other orders: empty
>
> Allocate 1 page (order 0):
>   Order 4 has no order-0 blocks. Split:
>     Order 4: empty
>     Order 3: [page 8-15]
>     Order 2: [page 4-7]
>     Order 1: [page 2-3]
>     Order 0: [page 1]    <-- return page 0 to caller
>
> Allocate another page:
>     Order 0: empty        <-- return page 1 to caller
>
> Free page 1:
>     Order 0: [page 1]
>     Page 1's buddy is page 0. Is page 0 free? No (it's allocated).
>     So page 1 stays at order 0.
>
> Free page 0:
>     Order 0: [page 0, page 1]
>     Page 0's buddy is page 1. Is page 1 free? Yes!
>     Merge: remove both from order 0, add [page 0-1] to order 1.
>     Page 0-1's buddy is page 2-3. Is it free? Yes (still on order-1
>     list from the original split).
>     Merge: promote to order 2 as [page 0-3].
>     Page 0-3's buddy is page 4-7. Free? Yes (still on order-2 list).
>     Merge: promote to order 3 as [page 0-7].
>     Page 0-7's buddy is page 8-15. Free? Yes (still on order-3 list).
>     Merge: promote to order 4 as [page 0-15].
> ```
>
> The cascading merge restores the original contiguous block. This is
> how the buddy system combats external fragmentation: freed adjacent
> blocks always merge, keeping large contiguous regions available.

### NUMA-aware allocation

On multi-socket servers, physical memory is not uniform. Each CPU
socket has "local" memory that it can access fast, and "remote" memory
on other sockets that's slower (3-5x latency difference). This is
**NUMA** -- Non-Uniform Memory Access.

Linux's allocator is NUMA-aware: each NUMA node (roughly: each socket)
has its own buddy allocator and free lists. When you allocate memory,
the kernel prefers pages from the local node. The `numactl` command
and `mbind()`/`set_mempolicy()` system calls let userspace control
which node's memory to use.

Our single-hart QEMU setup has no NUMA -- all memory is equally
accessible.

### Zones

Linux divides physical memory into **zones** based on address ranges:

| Zone | Address range | Purpose |
|------|-------------|---------|
| `ZONE_DMA` | 0 - 16 MB | Legacy ISA DMA (24-bit addresses only) |
| `ZONE_DMA32` | 0 - 4 GB | 32-bit DMA-capable devices |
| `ZONE_NORMAL` | Above 4 GB on 64-bit | Regular allocations |

Some hardware devices can only DMA to low physical addresses (e.g.,
old ISA sound cards can only access the first 16 MB). The zone system
ensures such devices get memory in their addressable range. Each zone
has its own buddy allocator.

We don't need zones -- QEMU's `virt` machine has all RAM in one
contiguous range, and we have no DMA devices.

---

## Part 8: Safety and Debugging

### What can go wrong

The three most common bugs in page allocator code:

**1. Double free.** Freeing the same page twice puts it on the free
list twice. Then two `kalloc()` calls return the same page, and two
different parts of the kernel use overlapping memory. This is
catastrophic and hard to debug because the corruption happens long
after the double free.

With our `struct page` array, we can actually *detect* double frees:
if `kfree()` is called on a page whose refcount is already 0, it's a
double free. This is a real advantage over xv6's bare free list, where
detection would require an O(n) list scan.

**2. Use after free.** Reading or writing a page after freeing it.
The junk fill (memset to 1) helps detect this -- the stale data is
replaced by conspicuous garbage.

**3. Memory leak.** Allocating pages without ever freeing them. The
`nr_free` counter helps detect this: if it keeps decreasing and never
goes back up, something is leaking. In later phases, we'll check
`nr_free` before and after tests to ensure no pages are leaked.

### The `panic()` guard rails

Our `kfree()` has several panic checks:

| Check | Bug it catches | Example |
|-------|---------------|---------|
| Not page-aligned | `kfree((void *)0x80001001)` -- someone freed an interior pointer instead of the page start | Pointer arithmetic error: `kfree(ptr + offset)` where `ptr` was a `kalloc` result |
| Below free region | `kfree((void *)0x80000000)` -- someone is trying to free the kernel's code/data or the `struct page` array | Corruption in a data structure that stores page pointers |
| At or above `PHYS_STOP` | `kfree((void *)0x90000000)` -- address not backed by RAM | Uninitialized or corrupted pointer |
| Refcount != 1 | `kfree()` on a page with refcount 0 (double free) or > 1 (still shared) | COW page freed without decrementing refcount first |

Note that our "below free region" bound is higher than xv6's `pa < end`
-- we must protect both the kernel image *and* the `struct page` array
that sits between the kernel and the free pages.

The refcount check is something xv6 can't do -- it's one of the
immediate benefits of the `struct page` array.

---

## Part 9: Our Implementation Plan

### Files

```
kernel/
    include/
        kalloc.h          <-- NEW: struct page, kalloc/kfree/kinit declarations
    kalloc.c              <-- NEW: page allocator implementation
    main.c                <-- UPDATE: call kinit() at boot
    test/
        test_kalloc.c     <-- NEW: allocator tests
        run_tests.c       <-- UPDATE: add test_kalloc()
Makefile                  <-- UPDATE: add kalloc.o and test_kalloc.o
```

### `kalloc.h`

The public interface:

```c
struct page {
    uint16 refcount;
};

void  kinit(void);       // Initialize the allocator (call once at boot)
void *kalloc(void);      // Allocate one zeroed 4 KB page. Returns NULL if OOM.
void  kfree(void *pa);   // Free a page previously returned by kalloc().

struct page *pa_to_page(uint64 pa);  // Look up struct page for a physical address
```

`pa_to_page()` converts a physical address to its `struct page`
entry -- a simple array index computation (matching Linux's
`pfn_to_page()` pattern). It's the fundamental operation that makes
per-page metadata useful: given any physical address, you can find
its metadata in O(1).

### `kalloc.c` -- our version

```c
#include "types.h"
#include "riscv.h"
#include "mem_layout.h"
#include "string.h"
#include "kprintf.h"
#include "kalloc.h"

extern char _kernel_end[];

struct run {
    struct run *next;
};

static struct run *free_list;
static uint64 nr_free;

static struct page *pages;     // struct page array (one per physical page)
static uint64 nr_pages;        // total number of physical pages

static void free_range(void *pa_start, void *pa_end);

struct page *
pa_to_page(uint64 pa)
{
    uint64 idx = (pa - KERN_BASE) / PG_SIZE;
    return &pages[idx];
}

void
kinit(void)
{
    nr_pages = (PHYS_STOP - KERN_BASE) / PG_SIZE;

    // Place struct page array right after the kernel image.
    pages = (struct page *)PG_ROUND_UP((uint64)_kernel_end);
    memset(pages, 0, nr_pages * sizeof(struct page));

    // Free pages start after the struct page array.
    char *free_start = (char *)PG_ROUND_UP(
        (uint64)pages + nr_pages * sizeof(struct page));
    free_range(free_start, (void *)PHYS_STOP);

    kprintf("kinit: %d free pages (%d KB), page array = %d KB\n",
            nr_free, nr_free * (PG_SIZE / 1024),
            (nr_pages * sizeof(struct page)) / 1024);
}

static void
free_range(void *pa_start, void *pa_end)
{
    char *p = (char *)PG_ROUND_UP((uint64)pa_start);
    for (; p + PG_SIZE <= (char *)pa_end; p += PG_SIZE) {
        pa_to_page((uint64)p)->refcount = 1;  // kfree expects refcount == 1
        kfree(p);
    }
}

void
kfree(void *pa)
{
    if ((uint64)pa % PG_SIZE != 0)
        panic("kfree: address not page-aligned");
    if ((uint64)pa < PG_ROUND_UP((uint64)pages + nr_pages * sizeof(struct page)))
        panic("kfree: address in reserved region");
    if ((uint64)pa >= PHYS_STOP)
        panic("kfree: address at or above PHYS_STOP");

    struct page *pg = pa_to_page((uint64)pa);
    if (pg->refcount != 1)
        panic("kfree: refcount is not 1");
    pg->refcount = 0;

    memset(pa, 1, PG_SIZE);

    struct run *r = (struct run *)pa;
    r->next = free_list;
    free_list = r;
    nr_free++;
}

void *
kalloc(void)
{
    struct run *r = free_list;
    if (r) {
        free_list = r->next;
        nr_free--;
        memset((char *)r, 0, PG_SIZE);
        struct page *pg = pa_to_page((uint64)r);
        pg->refcount = 1;
    }
    return (void *)r;
}
```

### Differences from xv6

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Per-page metadata | None | `struct page` array with `uint16 refcount` |
| `struct run` | Only data structure | Still used for free list threading; `struct page` tracks state externally |
| `kinit()` | `initlock` + `freerange` | Bootstrap `struct page` array + `free_range` + print stats |
| `kfree()` validation | `panic("kfree")` for all errors | Separate panics + refcount check (catches double free) |
| `kfree()` fill | `memset(pa, 1, PGSIZE)` | `memset(pa, 1, PG_SIZE)` (same pattern) |
| `kalloc()` fill | `memset(r, 5, PGSIZE)` (junk) | `memset(r, 0, PG_SIZE)` (zero) |
| `kalloc()` metadata | None | Sets `refcount = 1` |
| Free list variable | `kmem.freelist` (in struct with lock) | `static struct run *free_list` |
| Lock | `spinlock` (acquire/release) | None (single hart) |
| Page counter | None | `static uint64 nr_free` |
| Address→metadata | Impossible | `pa_to_page()` -- O(1) array index |
| Boot diagnostic | None | Prints free count, KB, array size |
| Linker symbol | `end` | `_kernel_end` |
| Test coverage | None | `test_kalloc.c` |
| Double free detection | No | Yes (refcount == 0 on free → panic) |

> **Note on `free_range()` and refcount.** During initialization,
> `free_range()` calls `kfree()` on pages that were never `kalloc()`'d
> -- their refcount is 0, not 1. But our `kfree()` panics if refcount
> != 1. So `free_range()` sets `refcount = 1` before each `kfree()`
> call, giving `kfree()` the precondition it expects. This avoids
> needing a separate "boot-only" free path.

### Updating `kmain()`

We add one line to `kmain()`:

```c
void
kmain(void) {
    uart_init();
    csrw(stvec, (uint64)kernel_vec);
    csrw(sie, csrr(sie) | SIE_SSIE);
    csrw(sstatus, csrr(sstatus) | SSTATUS_SIE);

    kprintf("\nbobchouOS is booting...\n");
    ...

    kinit();   // <-- NEW: initialize the page allocator

    ...
}
```

The `kinit()` call must come after `uart_init()` (so we can print)
and after setting `stvec` (so if `kinit()` triggers an exception,
the trap handler is ready). It should come before anything that needs
to allocate pages (nothing yet, but Phase 4's page table setup will).

### Expected boot output

```
bobchouOS is booting...
running in S-mode
sstatus=0x8000000200006022
kernel: 0x80000000 .. 0x8000XXXX (NNNNN bytes)
kinit: 32739 free pages (130956 KB), page array = 64 KB

timer interrupts enabled, waiting for ticks...
timer: 1 seconds
timer: 2 seconds
...
```

---

## Part 10: Testing the Allocator

### What to test

Key scenarios:

**Basic functionality:**
- `kalloc()` returns a non-NULL, page-aligned address
- The returned address is within the free region (above struct page
  array, below `PHYS_STOP`)
- The returned page is zeroed (every byte is 0)
- `kfree()` followed by `kalloc()` returns the same page (LIFO)

**Refcount tracking:**
- After `kalloc()`, the page's refcount is 1
- After `kfree()`, the page's refcount is 0
- `pa_to_page()` returns the correct entry for a given address

**Exhaustion:**
- Allocating all pages eventually returns NULL
- After freeing one page, `kalloc()` succeeds again

**Reuse:**
- A freed page can be reallocated
- The reallocated page is zeroed (not still filled with junk from
  `kfree()`)

**Invariants:**
- `nr_free` reflects the actual count (decrements on alloc, increments
  on free)

### Test strategy

We can't easily test `kfree()` panics (calling with bad addresses)
because our `panic()` halts the kernel. The panic paths are simple
enough to verify by inspection. We focus on testing the happy paths
and edge cases that don't panic.

> **Testing allocator exhaustion.** Allocating all ~32,000 pages in a
> test is feasible but slow (each allocation zeros 4 KB). We won't
> exhaustively drain the pool in tests -- we'll allocate a handful of
> pages, verify their properties, free them back, and check that the
> count returns to its original value.

---

## What's Next

After reading this lecture:

1. **Q&A** -- ask about anything unclear
2. **Skeleton commit** -- I'll create `kalloc.h`, `kalloc.c` (with
   TODOs), `test_kalloc.c`, and Makefile updates
3. **You implement** -- fill in the TODOs
4. **Review commit** -- I review and polish

---

## Quick Reference

### API

| Function | Signature | Description |
|----------|-----------|-------------|
| `kinit()` | `void kinit(void)` | Initialize allocator. Call once in `kmain()`. |
| `kalloc()` | `void *kalloc(void)` | Allocate one zeroed 4 KB page (refcount set to 1). Returns NULL if OOM. |
| `kfree()` | `void kfree(void *pa)` | Free a page (refcount must be 1). Panics on double free. |
| `pa_to_page()` | `struct page *pa_to_page(uint64 pa)` | Look up the `struct page` for a physical address. O(1). |

### Data structures

```c
struct page {
    uint16 refcount;          // 0 = free, 1 = allocated, >1 = shared (COW)
};

struct run {
    struct run *next;         // free list link (lives inside the free page)
};

static struct page *pages;    // struct page array (one per physical page)
static struct run *free_list;  // head of free list
static uint64 nr_free;        // number of free pages
```

### `kfree()` behavior

1. Validate: page-aligned, in free region, below `PHYS_STOP`
2. Check refcount == 1 (catches double free and shared-page mistakes)
3. Set refcount to 0
4. Fill page with `0x01` (junk -- catch dangling pointers)
5. Prepend to free list

### `kalloc()` behavior

1. Pop head of free list
2. If non-NULL: zero the page, set refcount to 1
3. Return pointer (or NULL if out of memory)

### Memory layout with allocator

```
0x80000000  +------------------+ <-- KERN_BASE / _kernel_start
            |  .text           |
            |  .rodata         |
            |  .data           |
            |  .bss            |
            |  stack (16 KB)   |
            +------------------+ <-- _kernel_end
            |  (round up)      |
            +------------------+ <-- PG_ROUND_UP(_kernel_end)
            |  struct page[N]  |     32,768 entries * 2 bytes = 64 KB
            +------------------+
            |  (round up)      |
            +==================+ <-- free_start
            |  page 0          |     \
            |  page 1          |      |
            |  page 2          |      |  Free pages managed by
            |  ...             |      |  the free list
            |  page N-1        |     /
            +==================+ <-- PHYS_STOP (0x88000000)
```

### Files changed in Round 3-2

| File | Change |
|------|--------|
| `kernel/include/kalloc.h` | **NEW** -- `struct page`, `kinit`, `kalloc`, `kfree`, `pa_to_page` |
| `kernel/kalloc.c` | **NEW** -- allocator implementation with struct page array |
| `kernel/main.c` | **UPDATE** -- call `kinit()` |
| `kernel/test/test_kalloc.c` | **NEW** -- allocator tests |
| `kernel/test/run_tests.c` | **UPDATE** -- register `test_kalloc()` |
| `Makefile` | **UPDATE** -- add `kalloc.o`, `test_kalloc.o` |

### Constants used

| Constant | Value | Defined in | Used for |
|----------|-------|-----------|----------|
| `PG_SIZE` | 4096 | `riscv.h` | Fill size, loop increment |
| `PG_ROUND_UP(a)` | -- | `riscv.h` | Align addresses to page boundary |
| `KERN_BASE` | `0x80000000` | `mem_layout.h` | Base for `pa_to_page()` index computation |
| `PHYS_STOP` | `0x88000000` | `mem_layout.h` | End of free region |
| `_kernel_end` | (linker) | `linker.ld` | Start of struct page array |

### Feature roadmap

| Phase | Feature | `struct page` field used |
|-------|---------|------------------------|
| 3 (now) | Basic alloc/free | `refcount` (0 or 1) |
| 6 | COW fork | `refcount` (0, 1, or >1) |
| Future | Swap/reclaim | Add `flags`, LRU list pointers |
| Future | Shared mmap | `refcount` (already present) |
| Future | Reverse mapping | Add `mapping`, `index` |
