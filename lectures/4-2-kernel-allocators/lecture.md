# Lecture 4-2: Kernel Memory Allocators (Buddy + Slab)

> **Where we are**
>
> Phase 4, Round 1 is complete. The kernel has working virtual memory:
> a 3-level Sv39 page table identity-maps the kernel text, data, MMIO
> devices, and all free RAM. `walk()` descends the tree,
> `map_pages()` installs leaf PTEs, and `vm_enable_paging()` writes
> `satp` to turn paging on. 45 tests pass. Every memory access now
> goes through the MMU.
>
> But the kernel only has one way to allocate memory: `kalloc()`, which
> hands out **4 KB pages**. That's the right tool for page tables,
> process address spaces, and anything else that must be page-aligned.
> But not everything needs a full page. A `struct proc` might be 200
> bytes. A file descriptor table might be 64 bytes. A buffer for a
> device driver might be 128 bytes. Allocating a full 4 KB page for
> each of these wastes enormous amounts of memory — 95% of every page
> goes unused.
>
> This lecture builds a complete kernel memory allocation stack. We
> replace the flat free list in `kalloc` with a **buddy allocator**
> that supports multi-page contiguous allocations, then layer a
> **slab allocator** (`kmalloc` / `kmfree`) on top for efficient
> sub-page allocations of arbitrary size (32 to 2048 bytes).
>
> By the end of this lecture, you will understand:
>
> - Why `kalloc()` alone isn't enough and what a heap allocator does
> - How `malloc()` works in general — the fundamental bookkeeping
>   problem
> - The free list approach (K&R-style) and its strengths and weaknesses
> - The size-class / pool approach (slab allocators) and why kernels
>   prefer it
> - External fragmentation vs. internal fragmentation — the core
>   tradeoff
> - Where slab metadata lives: external (in `struct page`) vs.
>   embedded (in the slab page itself)
> - The buddy allocator: how to allocate contiguous multi-page blocks
> - Why user-space large allocations (like `int a[1000000]`) don't
>   need contiguous physical pages
> - How Linux's SLUB allocator works (simplified)
> - bobchouOS implementation plan: buddy allocator in `kalloc.c`,
>   slab allocator in `kmalloc.c`
>
> **xv6 book coverage:** xv6 has neither a heap allocator nor a
> buddy allocator — it uses a flat free list of single pages for
> everything. This works for a teaching OS but wastes memory and
> can't allocate multi-page contiguous blocks. Our design draws from
> general allocator literature (Knuth, Bonwick's slab paper) and
> Linux's buddy + SLUB architecture, adapted for simplicity.

---

## Part 1: Why `kalloc()` Alone Isn't Enough

### The problem: granularity

`kalloc()` returns a 4096-byte page. Every call, no matter what you
need the memory for. Need 32 bytes for a small struct? You get 4096.
Need 200 bytes for a process descriptor? 4096. Need 8 bytes for a
single pointer? 4096.

This is called **internal fragmentation** — you asked for N bytes
but got a page-sized container, and the remaining `4096 - N` bytes
inside that container are wasted. If the kernel has 100 active
process structs (200 bytes each), that's 100 pages = 400 KB, of
which only 20 KB is actually used. The other 380 KB is wasted.

### What xv6 does (and why it works for xv6)

xv6 allocates everything at page granularity. Every `struct proc`
gets its own page. Every pipe buffer is a page. This is simple and
correct, but only because xv6 runs a tiny number of processes on a
system with plenty of RAM relative to its needs. xv6 never runs out
of memory in practice, so the waste doesn't matter.

But as an OS grows — more processes, more open files, more driver
buffers, more data structures — the waste adds up. A system with
1000 open files, each needing a 128-byte `struct file`, would waste
nearly 4 MB under a page-per-object scheme. That's why every
production kernel has a sub-page allocator.

### The solution: a layered allocator stack

The fix isn't a single allocator — it's two layers that handle
different granularities:

1. **Buddy allocator** — replaces our flat free list in `kalloc`.
   Manages physical pages in power-of-two blocks (1 page, 2 pages,
   4 pages, ... up to 1024 pages). Handles both single-page and
   multi-page contiguous allocations. Covered in Part 7.

2. **Slab allocator** (`kmalloc` / `kmfree`) — sits on top of the
   buddy allocator. Requests whole pages and carves them into
   fixed-size slots for sub-page objects (32 to 2048 bytes).
   Covered in Parts 3-6.

Together they form a complete stack:

```
  ┌──────────────────────────────────────┐
  │  Kernel code                         │
  │  kmalloc(200) → sub-page object      │
  │  kalloc_pages(2) → 8 KB contiguous   │
  └──────────────┬───────────────────────┘
                 │
  ┌──────────────v───────────────────────┐
  │  Slab allocator (kmalloc / kmfree)   │
  │  7 size classes (32 - 2048 bytes)    │
  │  Carves pages into fixed-size slots  │
  └──────────────┬───────────────────────┘
                 │
  ┌──────────────v───────────────────────┐
  │  Buddy allocator (kalloc / kfree)    │
  │  Power-of-two page blocks            │
  │  Orders 0-10 (4 KB - 4 MB)           │
  └──────────────┬───────────────────────┘
                 │
  ┌──────────────v───────────────────────┐
  │  Physical memory (DRAM)              │
  └──────────────────────────────────────┘
```

When the slab allocator needs more memory, it calls `kalloc()` to
get a fresh 4 KB page and carves it up. When an entire page's worth
of sub-page objects are freed, it returns the page to the buddy
allocator via `kfree()`. This is the same architecture Linux uses
— slab on top of buddy — just much simpler.

### kmalloc is not malloc — two separate worlds

If you've used `malloc()` and `free()` in C user programs, `kmalloc`
is the same idea — but it lives **exclusively inside the kernel** and
serves only the kernel's own data structures. User-space `malloc`
(in libc) is a completely separate allocator that sits on top of the
OS's page allocator via `mmap` or `brk` syscalls. The two never mix,
not now and not in the future:

| | Kernel heap (`kmalloc`) | User heap (`malloc`) |
|---|---|---|
| Runs in | S-mode, kernel address space | U-mode, per-process address space |
| Managed by | Kernel (`kmalloc.c`) | libc (inside the process) |
| Backed by | `kalloc()` pages | Pages the kernel mapped into the process |
| Lifetime | Kernel boot → shutdown | Process start → exit |
| Who can see it | Kernel only | That one process only |

The kernel's role in user-space allocation is limited: when a process
needs more heap memory, it asks the kernel via a syscall (`sbrk` or
`mmap`). The kernel responds by allocating physical pages (via
`kalloc`) and mapping them into the process's page table. From that
point on, the process's own `malloc` library manages those pages —
the kernel never touches the user heap directly.

```
  User calls malloc(100):

  ┌──────────────────────────────────────────┐
  │  User process (U-mode)                   │
  │                                          │
  │  libc malloc(100)                        │
  │    → has free space? yes → return ptr    │
  │    → no free space?                      │
  │        sbrk(4096) syscall ───────────┐   │
  │        ← kernel returns new page ←───┘   │
  │        carve 100 bytes, return ptr       │
  └──────────────────────────────────────────┘

  ┌──────────────────────────────────────────┐
  │  Kernel (S-mode) handling sbrk syscall   │
  │                                          │
  │  kalloc() → physical page                │
  │  map page into process's page table      │
  │  return to user                          │
  │                                          │
  │  (kmalloc is NOT involved here)          │
  └──────────────────────────────────────────┘
```

This separation is fundamental to isolation: the kernel and each
user process have independent heaps in independent address spaces.
A bug in a user program's heap can't corrupt the kernel's data
structures, and vice versa.

---

## Part 2: How malloc Works — The Fundamental Problem

### The bookkeeping challenge

The core problem of any allocator is: **given a pointer returned by
`free()`, how do you know how much memory to reclaim?**

When the caller invokes `kmalloc(200)`, you hand back a pointer.
Later they call `kmfree(ptr)`. You now need to know that `ptr` points to
a 200-byte block, not 64 or 4096. And you need to find a way to
reuse that memory for future allocations.

This requires bookkeeping: metadata that tracks which regions are
allocated, which are free, and how large each one is. Where you
store this metadata and how you organize it defines the allocator's
design.

### Approach 1: Inline headers (K&R-style free list)

The classic approach from Kernighan & Ritchie's "The C Programming
Language" (Section 8.7) stores metadata directly in front of each
block:

```
  One heap block:
  ┌────────────────┬──────────────────────────────────┐
  │  Header        │  User data (N bytes)             │
  │  (size, next)  │  ← pointer returned to caller    │
  └────────────────┴──────────────────────────────────┘
       8-16 bytes
```

`malloc(N)` returns a pointer to the user data area; the header sits
just before it. `free(ptr)` subtracts the header size from `ptr` to
find the metadata: the block's size and a pointer to thread it into
a free list.

Free blocks are linked together in a single list. `malloc` walks the
list looking for a block large enough to satisfy the request. If no
block fits, it asks the OS for more memory.

```
  Free list (K&R style):
  ┌──────────┐     ┌──────────┐     ┌──────────┐
  │ 64 bytes │ --> │ 128 bytes│ --> │ 256 bytes│ --> NULL
  │  (free)  │     │  (free)  │     │  (free)  │
  └──────────┘     └──────────┘     └──────────┘
```

When the caller frees a block, the allocator adds it back to the
free list. To reduce fragmentation, adjacent free blocks can be
**coalesced** (merged) into one larger block.

**Strengths of this approach:**

- Simple to implement (50-100 lines of code)
- No wasted memory for unused size classes — each block is exactly
  the size requested (plus header)
- Coalescing can recover contiguous free regions

**Weaknesses:**

- **External fragmentation:** Over time, the heap becomes a
  patchwork of allocated and free blocks. You might have 1000 bytes
  free across 10 scattered 100-byte blocks, but can't satisfy a
  single 200-byte allocation. The total free memory is sufficient,
  but no single contiguous piece is large enough.

  ```
    External fragmentation — death by a thousand cuts:

    [USED 64][FREE 100][USED 200][FREE 100][USED 32][FREE 100]...

    Total free: 300 bytes. But largest single block: 100 bytes.
    A 200-byte allocation fails despite having enough total memory.
  ```

- **Linear search.** Walking the free list to find a block is O(n)
  in the number of free blocks. With many small free blocks, this
  gets slow.

- **Fragile to bugs.** The header is inline with user data. A
  buffer overflow in one allocation corrupts the next block's header,
  silently destroying the free list. These bugs are notoriously hard
  to track down because the corruption doesn't crash immediately —
  it crashes later when `malloc` or `free` tries to follow a
  corrupted pointer.

- **Poor cache behavior.** A CPU cache works in **cache lines** —
  fixed-size chunks (typically 64 bytes) that are the smallest unit
  the cache loads from memory. When the CPU reads one byte, the
  hardware fetches the entire 64-byte cache line containing it. If
  the next byte the CPU needs happens to be in the same cache line,
  it's already there — a **cache hit** (~1-2 ns from L1). If it's
  in a different line that isn't cached, the CPU must fetch it from
  a slower level — a **cache miss** that costs 5-10 ns from L2,
  20-40 ns from L3, or ~100 ns from DRAM. That's up to 50-100x
  slower than an L1 hit.

  This is why **spatial locality** matters: data that is accessed
  together should be stored together. If all `struct proc` objects
  are packed next to each other on the same pages, iterating over
  them produces mostly cache hits — each cache line fetch brings in
  parts of multiple proc structs. But in a K&R allocator, a 200-byte
  proc struct might be followed by a 64-byte file descriptor, then a
  128-byte buffer, then another proc struct. The proc structs are
  scattered across many cache lines and many pages, so iterating over
  them causes a cache miss on nearly every access:

  ```
    K&R heap — objects interleaved by allocation order:

    Page N:   [proc A][file 1][buf X][proc B][file 2][buf Y]...
    Page N+1: [buf Z][proc C][file 3][proc D][buf W]...

    Iterating over all procs → cache miss on each one.
  ```

  ```
    Slab heap — same-type objects packed together:

    Page N:   [proc A][proc B][proc C][proc D][proc E]...
    Page M:   [file 1][file 2][file 3][file 4][file 5]...

    Iterating over all procs → mostly cache hits.
  ```

  This difference is significant in practice. On modern hardware,
  a cache miss to DRAM costs ~100 nanoseconds — 100x slower than a
  cache hit. Consider a typical scheduler time slice of ~4 ms
  (4,000,000 ns). If the scheduler walks 1000 scattered proc structs
  and each access misses the cache, that's 1000 × 100 ns = 100
  microseconds — 2.5% of the time slice gone just reading the data
  structure, before any useful work happens. With slab-packed proc
  structs on ~50 contiguous pages, most accesses hit L1/L2 and the
  same loop takes ~2-5 microseconds — a 20-50x improvement from data
  layout alone. Add TLB misses (each scattered page may need a
  page table walk too), and the gap widens further. This is one of
  the main reasons production kernels use slab allocators.

> **Historical note.** The K&R allocator is the original textbook
> `malloc`. It was state-of-the-art in the 1970s when memory was
> tiny and simplicity mattered above all else. Modern general-purpose
> allocators (glibc's ptmalloc2, jemalloc, mimalloc) are descendants
> of this idea but add bins/size classes, per-thread caches, and
> arena-based organization to address its weaknesses. They're
> thousands of lines of code.

### Approach 2: Size-class pools (slab-style)

The observation behind slab allocators is: **most kernel allocations
fall into a small number of fixed sizes.** Process structs are
always the same size. File descriptors are always the same size.
Pipe buffers are always the same size. Rather than maintaining a
single free list of variable-sized blocks, maintain **separate pools
for each size.**

```
  Size class 32 bytes:     Size class 64 bytes:     Size class 256 bytes:
  ┌──────────┐             ┌──────────┐             ┌───────────┐
  │ page     │             │ page     │             │ page      │
  │ ┌──┐┌──┐ │             │ ┌────┐   │             │ ┌────────┐│
  │ │32││32│ │             │ │ 64 │   │             │ │  256   ││
  │ ├──┤├──┤ │             │ ├────┤   │             │ ├────────┤│
  │ │32││32│ │             │ │ 64 │   │             │ │  256   ││
  │ ├──┤├──┤ │             │ ├────┤   │             │ ├────────┤│
  │ │32││32│ │             │ │ 64 │   │             │ │  256   ││
  │ ├──┤├──┤ │             │ ├────┤   │             │ ├────────┤│
  │ │..││..│ │             │ │... │   │             │ │  ...   ││
  │ └──┘└──┘ │             │ └────┘   │             │ └────────┘│
  └──────────┘             └──────────┘             └───────────┘
  128 objects/page         64 objects/page          16 objects/page
```

Each size class has its own **slab** — one or more pages dedicated
to objects of that exact size. The page is divided into fixed-size
slots. A free list threads through the unused slots within each
slab.

`kmalloc(N)` rounds N up to the nearest size class, then pops a
slot from that class's free list. `kmfree(ptr)` determines which
size class the block belongs to and pushes it back onto the
corresponding free list.

**Strengths:**

- **No external fragmentation.** Every block in a slab is the same
  size. If a slot is free, it can satisfy any allocation for that
  size class. Fragmentation between different sizes is impossible
  because they live on different pages.

- **O(1) allocation and free.** Pop from or push to a free list.
  No searching.

- **Cache-friendly.** All objects of the same type are packed on the
  same pages. Iterating over process structs touches fewer cache
  lines.

- **Simple metadata.** You don't need per-block headers — the slot
  size is determined by which slab the pointer falls in. This means
  no inline headers to corrupt.

**Weaknesses:**

- **Internal fragmentation.** If you request 33 bytes and the
  nearest size class is 64, you waste 31 bytes per allocation.
  This is controlled by choosing size classes carefully.

- **Memory overhead from partially-filled slabs.** If a 32-byte
  slab has only 2 of 128 slots in use, the rest of the page is
  reserved for 32-byte allocations and can't be used for other
  sizes. This is more of a concern when there are many size classes
  with low utilization.

### The tradeoff: external vs. internal fragmentation

These two approaches represent a fundamental tradeoff:

| | K&R free list | Size-class pools |
|---|---|---|
| Block size | Exact (+ small header) | Rounded up to size class |
| Fragmentation type | **External** — free space scattered | **Internal** — waste inside each block |
| Allocation speed | O(n) — search free list | O(1) — pop from class list |
| Free speed | O(1) to O(n) — may coalesce | O(1) — push to class list |
| Metadata | Inline headers (fragile) | Slab-level (robust) |
| Implementation | Simple | Moderate |

For a kernel, the size-class approach wins because:

1. Kernel objects are mostly fixed-size — the rounding waste is
   small
2. O(1) allocation matters — the kernel allocates in interrupt
   handlers and on hot paths
3. No external fragmentation means the allocator doesn't degrade
   over time
4. No inline headers means fewer corruption bugs

This is why every major OS kernel uses a variant of slab allocation:
Linux (SLAB → SLUB), FreeBSD (UMA), Windows (pool allocator with
lookaside lists), Solaris (the original slab allocator by Jeff
Bonwick, 1994).

> **Jeff Bonwick's slab allocator (1994).** The slab allocator was
> introduced in Solaris and described in the paper "The Slab
> Allocator: An Object-Caching Kernel Memory Allocator" (USENIX
> Summer 1994). The key insight was that kernel objects are created
> and destroyed frequently, and most are the same size — so the
> allocator should cache constructed objects rather than raw memory.
> Bonwick's design influenced every kernel allocator since. The name
> "slab" refers to a group of pages dedicated to objects of one type.

---

## Part 3: Size Classes — How to Choose Them

### The design question

If we're going to round every allocation up to a fixed size class,
which sizes should we pick? Too few classes and the rounding waste
(internal fragmentation) is large. Too many classes and we waste
memory on partially-filled slabs.

### Power-of-two classes

The simplest and most common scheme: each size class is double the
previous one.

| Size class | Slots per page (4096 / size) |
|---|---|
| 32 | 128 |
| 64 | 64 |
| 128 | 32 |
| 256 | 16 |
| 512 | 8 |
| 1024 | 4 |
| 2048 | 2 |

Three things to explain about this table: why 32 is the floor, why
2048 is the ceiling, and why powers of two.

**Why 32 minimum.** Each free slot must hold a pointer to the next
free slot (8 bytes on RV64). A 16-byte class would work
mechanically, but most kernel objects are already 16-24 bytes by
the time they have a pointer field and a type tag — so a 16-byte
class would see little use. Starting at 32 gives every slot enough
room for the free-list pointer and still leaves useful payload.

**Why 2048 maximum.** A slab page is 4096 bytes. The 2048 class
yields 2 slots per page — that's the smallest class where multiple
objects still share a page. Going higher makes the slab degenerate:
a 4096-byte class gives 1 slot per page, which is just `kalloc()`
with extra bookkeeping. So anything above 2048 bytes skips the slab
and goes straight to `kalloc()` (the "big-alloc" path in Part 6).

**Why powers of two.** Three reasons work together:

1. Every power-of-two size divides 4096 evenly — zero wasted bytes
   at the end of each slab page, as the table above shows.
2. Finding the right class is trivial: `size = 1 << class_index`.
3. Worst-case internal fragmentation is bounded at just under 50%
   (requesting `2^k + 1` rounds up to `2^(k+1)`). In practice,
   kernel objects tend to cluster near power-of-two boundaries, so
   the average waste is much less.

---

## Part 4: Slab Internals — How a Single Slab Works

### Slab structure

Each slab is a single 4 KB page obtained from `kalloc()`. The page
is divided into equal-sized slots. We need metadata to track which
class this slab belongs to, how many slots are allocated, and where
the free slots are. But where does this metadata live?

### Where to store slab metadata

**Option A: External — in `struct page`.** We already have a
per-page metadata array from Phase 3 (`struct page` with a
`refcount` field). Every physical page in the system has one. We can
expand `struct page` with slab-specific fields using a **union** —
since a page can only be one thing at a time (free, page table,
slab, user page), the different field sets overlay each other:

```c
struct page {
    uint16 refcount;          // always valid
    uint8 order;              // buddy allocator order (Part 7)
    union {
        struct {              // when page is used as a slab
            uint8 class_idx;
            uint16 nr_alloc;
            void *free_list;
            struct page *next_slab;
        } slab;
        // future: page table metadata, user page metadata, ...
    };
};
```

(The `order` field is for the buddy allocator, introduced in Part 7.
The full layout is discussed in Part 9.)

Given a pointer into a slab, we find its metadata via `pa_to_page()`
— the same function we already have. No data is stored inside the
slab page itself, so **every byte of the page is usable for slots.**

This is the approach Linux and FreeBSD use — the production-kernel
standard. (Part 8 covers how Linux's SLUB implements this in more
detail.)

**Option B: Embedded — inside the slab page.** Store a small header
at the beginning of the slab page itself. Given a pointer, find the
header by masking off the page offset: `header = ptr & ~(PG_SIZE - 1)`.
Simple, but has real downsides:

- **No unified page state.** With external metadata, `struct page`
  is the single source of truth for "what is this physical page?"
  — free, slab, page table, user page. Embedded headers scatter
  that information into the pages themselves, so answering "what
  kind of page is this?" requires knowing which subsystem owns it
  before you can even find the metadata. The union approach gives
  every page a uniform lookup path.
- **Can't grow with the system.** When we later add page table
  tracking, user page reverse mapping, or LRU lists, those all
  need per-page metadata too. With `struct page`, we add a union
  variant. With embedded headers, each subsystem invents its own
  in-page header format — no shared infrastructure.
- **Wastes slab space.** The header consumes at least one slot per
  page (more for large classes where alignment padding is needed).

> **Embedded headers in the real world.** Embedded RTOSes (FreeRTOS,
> Zephyr, NuttX) commonly use embedded headers. These kernels run on
> microcontrollers with limited RAM (sometimes 64-256 KB total) and
> often **no MMU** — no virtual memory, no paging, no per-page
> metadata array. Memory management is a flat heap: the OS gets a
> chunk of RAM and carves it up directly. The allocator embeds
> headers in the heap itself because there's nowhere else to put
> them — no `struct page`, no external tables. It's not "no kernel"
> — it's "no MMU, no paging, no per-page metadata, so embedded
> header is the only simple option."

**bobchouOS uses Option A.** We already have `struct page` and
`pa_to_page()`, so the infrastructure is free. This matches how
production kernels do it, maximizes slab utilization, and gives
`struct page` its natural role as the single source of truth for
"what is this physical page being used for?" — a union that grows
as we add subsystems.

The cost: `struct page` grows from 2 bytes to ~32 bytes. With ~32K
pages, the page array goes from ~64 KB to ~1 MB — about 0.8% of
our 128 MB RAM. As we discussed in Lecture 3-2, Linux's `struct
page` is ~64 bytes (1.6% overhead), so we're well within the
accepted range for production kernels.

### Slab page layout

With external metadata, the entire slab page is devoted to slots:

```
  A slab page for 64-byte class (4096 bytes):

  ┌──────────────────────────────────────────────┐
  │ Slot 0   [object or free-list link]   64B    │
  │ Slot 1   [object or free-list link]   64B    │
  │ Slot 2   [object or free-list link]   64B    │
  │ ...                                          │
  │ Slot 63  [object or free-list link]   64B    │
  └──────────────────────────────────────────────┘

  Metadata lives in struct page (external):
    page->slab.class_idx  = 1
    page->slab.nr_alloc   = 3
    page->slab.free_list  → Slot 1
    page->slab.next_slab  → next slab in class list
```

Every power-of-two slot size divides 4096 evenly, so zero bytes are
wasted at the end of the page — the full `PG_SIZE / slot_size` slots
are usable.

### The embedded free list

Free slots within a slab are linked together using the slot memory
itself — the same trick `kalloc` uses for free pages. Each free
slot's first 8 bytes hold a pointer to the next free slot:

```
  Slab for 64-byte class:

  [Slot 0: ALLOCATED - user data 64 bytes ]
  [Slot 1: free → points to slot 3        ]
  [Slot 2: ALLOCATED - user data 64 bytes ]
  [Slot 3: free → points to slot 5        ]
  [Slot 4: ALLOCATED - user data 64 bytes ]
  [Slot 5: free → NULL (end of list)      ]
  ...

  page->slab.free_list → Slot 1 → Slot 3 → Slot 5 → NULL
```

This is efficient: free slots cost zero extra memory because we
reuse the slot's own bytes for the link pointer. Allocated slots
contain user data — the link pointer is overwritten when the slot
is handed out.

### Allocation within a slab

`kmalloc(N)` for a size that maps to this slab:

1. Pop the first slot from the slab's free list
   (`page->slab.free_list`)
2. Increment `page->slab.nr_alloc`
3. Return the slot pointer to the caller

O(1). No searching, no splitting.

### Free within a slab

`kmfree(ptr)`:

1. Find the metadata: `pa_to_page((uint64)ptr)` — looks up the
   `struct page` entry for this address
2. Push the slot onto `page->slab.free_list`
3. Decrement `page->slab.nr_alloc`
4. If `nr_alloc` reaches 0, the entire slab is empty — return the
   page to `kalloc` via `kfree()`

O(1). The `pa_to_page()` lookup is a simple array index — same
cost as the page-mask trick, but using the infrastructure we already
built in Phase 3.

---

## Part 5: The Global Picture — Tying Slabs Together

### Per-class slab lists

The allocator maintains an array of **size class descriptors**, one
per size class. Each descriptor points to a linked list of slabs for
that class:

```
  size_classes[]:

  [0] class 32   → slab A (14 free) → slab B (0 free) → NULL
  [1] class 64   → slab C (60 free) → NULL
  [2] class 128  → slab D (30 free) → NULL
  [3] class 256  → slab F (16 free) → NULL
  [4] class 512  → slab E (6 free) → NULL
  [5] class 1024 → slab G (4 free) → NULL
  [6] class 2048 → slab H (2 free) → NULL
```

Every class always has at least one slab (pre-allocated by
`kmalloc_init`). Classes with more demand grow additional slabs.

When `kmalloc(N)` is called:

1. **Round up** N to the nearest size class. For example, 100 → 128.
2. **Find the class descriptor** for 128 bytes.
3. **Check the first slab** in the list. If it has free slots, pop
   one and return.
4. **If all slabs are full**, allocate a new slab page via
   `kalloc()`, initialize it (divide into slots, build the free
   list), prepend it to the class's slab list, and pop a slot.

### Finding the size class from a pointer (for kmfree)

When `kmfree(ptr)` is called, we need to know which size class this
allocation belongs to. Since slab metadata lives in `struct page`,
we look it up with:

```c
struct page *pg = pa_to_page((uint64)ptr);
int class = pg->slab.class_idx;
```

This is O(1) — `pa_to_page` is a simple array index into the page
metadata array we built in Phase 3.

### Returning empty slabs

When the last object in a slab is freed (`nr_alloc` drops to 0),
the slab is completely empty. We have three options:

1. **Keep it** — leave it on the class's slab list for future
   allocations. Fast, but the memory can't be used for other classes
   or returned to the page allocator.

2. **Free it immediately** — unlink it from the class list and call
   `kfree()`. Memory-efficient, but if this size class sees another
   allocation soon, we'll have to allocate a new slab from `kalloc`.

3. **Hybrid** — keep one empty slab per class (for fast reuse),
   free extras. Balances memory efficiency and performance.

bobchouOS uses option 3 (hybrid — keep one empty slab per class).
The benefit goes beyond caching: if every class always has at least
one slab, the allocation fast path is simply "pop from the first
slab's free list" — no need to check "does a slab exist?" or handle
the cold-start case where the class has no slabs at all. The code
is cleaner: `kmalloc_init` pre-allocates one slab per class, and
the "no slab" path only triggers if the single kept slab is full
and needs a second. When a slab empties, we only free it if the
class already has another empty slab to fall back on.

> **Production kernels are even more aggressive.** Linux SLUB keeps
> entire partial lists (slabs that are neither full nor empty)
> around indefinitely, and only returns pages to the buddy allocator
> when memory pressure triggers explicit shrinking. FreeBSD's UMA
> similarly holds empty slabs in a cache and only releases them under
> pressure or via a periodic reaper timer. Our "keep one" policy is
> the conservative end of what real kernels do — the right spot for
> a teaching OS where simplicity matters more than squeezing out
> every last allocation cycle.

### The full allocation path

```
  kmalloc(100):
    1. Round 100 → 128 (size class index 2)
    2. Look at size_classes[2].slab_list
    3. First slab has free slots?
       Yes → pop slot, return pointer
       No  → kalloc() a new page
             initialize as 128-byte slab
             prepend to size_classes[2].slab_list
             pop slot, return pointer
```

```
  kmfree(ptr):
    1. pa_to_page(ptr) → struct page
    2. Read page->slab.class_idx → index 2
    3. Push slot back onto page->slab.free_list
    4. Decrement page->slab.nr_alloc
    5. If nr_alloc == 0 and class already has another empty slab:
         unlink this slab from size_classes[2].slab_list
         kfree(slab page)
       (otherwise keep it — one empty slab per class is fine)
```

---

## Part 6: Handling Large Allocations

### The big-alloc path

When `kmalloc(N)` receives a request larger than 2048 bytes (our
largest slab class), it bypasses the slab allocator entirely and
goes straight to the buddy allocator (Part 7). The buddy allocator
works in **orders** — an order-N block is 2^N contiguous pages. The
request is rounded up to the nearest order, and `kalloc_pages(order)`
does the rest.

Since metadata lives in `struct page`, distinguishing big
allocations from slab allocations is easy — just set `class_idx`
to a sentinel value and record the buddy order:

1. `kmalloc(N)` where N > 2048: compute the order needed
   (e.g., N=8192 → 2 pages → order 1)
2. Call `kalloc_pages(order)` to get contiguous pages
3. Set `pa_to_page(ptr)->slab.class_idx = BIG_ALLOC` on the first
   page — the buddy order is already stored in `page->order` by
   the buddy allocator
4. Return the pointer — the **entire** block is usable (no embedded
   header to skip past)

Freeing:

1. `kmfree(ptr)` → `pa_to_page(ptr)`, read `class_idx`, see
   `BIG_ALLOC`
2. Read `page->order` to know the block size
3. Call `kfree_pages(ptr, page->order)`

This means `kmalloc` handles **any size** — from 1 byte to
megabytes. The caller never needs to think about which allocator
to use:

| Request size | Route |
|---|---|
| 1 – 2048 bytes | Slab allocator (size class) |
| 2049 – 4096 bytes | Buddy order 0 (1 page) |
| 4097 – 8192 bytes | Buddy order 1 (2 pages) |
| 8193 – 16384 bytes | Buddy order 2 (4 pages) |
| ... | ... |

`kmalloc(N)` always works. `kmfree(ptr)` always works. One
interface for all kernel allocations.

> **When to call kalloc_pages directly?** When you need explicit
> control over the buddy order or want to avoid the `BIG_ALLOC`
> metadata overhead. In practice, most kernel code should use
> `kmalloc`/`kmfree` for the uniform interface. Direct
> `kalloc_pages` is for low-level code that knows exactly what it
> needs — page tables, DMA setup, etc.

---

## Part 7: The Buddy Allocator — Multi-Page Allocations

Parts 3-6 described the slab allocator for sub-page objects. But
the slab allocator itself needs pages — and Part 6's big-alloc path
needs multi-page contiguous blocks. This is the buddy allocator's
job: the page-level layer that sits beneath everything else.

Our current `kalloc` uses a flat free list of single pages. The
buddy allocator replaces it, adding the ability to allocate
**power-of-two groups of contiguous pages** — something the flat
list cannot do.

When does the kernel need contiguous physical pages?

- **Big kmalloc** — `kmalloc(8192)` needs 2 contiguous pages
  (Part 6)
- **DMA buffers** — hardware devices access physical memory
  directly, bypassing the MMU. They need physically contiguous
  buffers.
- **Multi-page kernel stacks** — Linux uses 2-4 page kernel stacks
  under identity mapping where virtual = physical.
- **Huge pages** — a 2 MB superpage requires 512 contiguous physical
  pages by definition.

### But wait — doesn't virtual memory solve this?

For **user-space** allocations, yes. Consider `int a[1000000]` — a
4 MB array. The user program sees a contiguous virtual address
range, but the physical pages behind it can be completely scattered:

```
  User sees:              Physical reality:

  VA 0x10000  a[0..1023]    →  page at 0x80300000
  VA 0x11000  a[1024..2047] →  page at 0x80700000
  VA 0x12000  a[2048..3071] →  page at 0x80100000
  ...                          (scattered, doesn't matter)
```

The page table maps each 4 KB virtual page to its physical page
independently. The kernel allocates them one at a time via `kalloc()`
and installs the mappings via `map_pages()`. The user program never
knows the physical pages aren't contiguous — the MMU makes them
*virtually* contiguous, which is all software cares about.

This is one of the fundamental benefits of virtual memory: user-space
never needs contiguous physical memory, no matter how large the
allocation. The kernel just hands out scattered pages and lets the
page table stitch them together.

But for the **kernel itself**, under identity mapping (VA == PA), a
16 KB buffer really does need 4 adjacent physical pages — because
there's no page table indirection to paper over the gaps.

> **Linux's escape hatch.** Linux recently added **virtually-mapped
> kernel stacks** — kernel stacks backed by scattered physical pages,
> mapped to contiguous kernel virtual addresses. This eliminates
> one of the last reasons the kernel needed physically contiguous
> allocations. But DMA and huge pages still require it.

### How the buddy allocator works

The buddy allocator organizes free pages into power-of-two groups
called **orders**:

```
  order 0:  free blocks of 1 page    (4 KB)
  order 1:  free blocks of 2 pages   (8 KB)
  order 2:  free blocks of 4 pages   (16 KB)
  order 3:  free blocks of 8 pages   (32 KB)
  ...
  order 9:  free blocks of 512 pages (2 MB)
  order 10: free blocks of 1024 pages (4 MB)
```

Each order has its own free list. A block of order N contains 2^N
contiguous pages.

**Allocation** — requesting 2^N contiguous pages:

1. Check the order-N free list. If a block is available, remove it
   and return it. Done.
2. If empty, check order N+1. Take a larger block and **split** it:
   the lower half becomes the allocation, the upper half (the
   **buddy**) goes on the order-N free list for future use.
3. If order N+1 is also empty, go to N+2 and split twice. Repeat
   up to the maximum order.

```
  Allocating 2 pages (order 1), order-1 list empty:

  Order 2 free list has a 4-page block at 0x80100000:

  ┌──────────┬──────────┬──────────┬──────────┐
  │  Page 0  │  Page 1  │  Page 2  │  Page 3  │
  └──────────┴──────────┴──────────┴──────────┘
  0x80100000                        0x80104000

  Split into two order-1 blocks:

  ┌──────────┬───────────┐  ┌──────────┬──────────┐
  │  Page 0  │  Page 1   │  │  Page 2  │  Page 3  │
  │  (returned to caller)│  │  (put on order-1    │
  │                      │  │   free list)        │
  └──────────┴───────────┘  └──────────┴──────────┘
  0x80100000               0x80102000
```

**Freeing** — the elegant part:

1. Return the block to its order's free list.
2. Check if the block's **buddy** is also free. The buddy is the
   adjacent block of the same size — its address differs by exactly
   one bit:  `buddy_addr = addr XOR (PG_SIZE << order)`.
3. If the buddy is free, **merge** (coalesce) them into one block
   of order N+1. Remove the buddy from order N's free list, put the
   combined block on order N+1's free list.
4. Repeat — the merged block might have *its* buddy free too.
   Continue merging until the buddy is in-use or you reach the
   maximum order.

```
  Freeing pages at 0x80100000 (order 1):
  Check buddy at 0x80100000 XOR 0x2000 = 0x80102000

  Buddy is free! Merge into order-2 block at 0x80100000.
  Check buddy at 0x80100000 XOR 0x4000 = 0x80104000

  Buddy is in-use. Stop. Put merged block on order-2 list.
```

The XOR trick works because buddy pairs are always aligned to their
combined size. Two order-1 blocks (2 pages each) that are buddies
together form one order-2 block (4 pages), and that 4-page block is
4-page-aligned. The single bit that differs between their addresses
is exactly the bit that distinguishes "lower half" from "upper half"
of the parent block.

### Why "buddy"?

The name comes from the pairing: every block has exactly one
**buddy** — the other half it was split from (or could merge with).
Two buddies together reconstitute the parent block. It's a recursive
structure: at every order, blocks come in pairs.

### Buddy allocator properties

| Property | Details |
|---|---|
| Allocation | O(log N) worst case — may split through multiple orders |
| Freeing | O(log N) worst case — may merge through multiple orders |
| Fragmentation | Only power-of-two sizes → up to 50% internal waste |
| External frag | Merging prevents long-term external fragmentation |
| Metadata | Need to track order and free/in-use per block |

The 50% internal waste (requesting 3 pages gets 4) is the main
downside. Linux mitigates this by layering the slab allocator on
top — the slab handles sub-page objects efficiently, and the buddy
handles the page-level backing.

### Tracking block order

When `buddy_free(pa)` is called, we need to know the order (size)
of the block being freed. We store this in `struct page` as an
`order` field outside the union — buddy merging needs the order
regardless of how the page is used. (The full `struct page` layout
is shown in Part 9.)

### bobchouOS buddy allocator

Our implementation is simple — the minimum viable buddy:

- **Max order: 10** (1024 pages = 4 MB). Easy to remember: 4 KB ×
  1024 = 4 MB. This matches Linux's maximum allocatable order of 10
  (1024 pages). Linux makes `MAX_ORDER` configurable via kernel
  config — we hard-code it.
- **11 free lists** (orders 0 through 10), stored as a static array.
- `buddy_alloc(order)` — find or split, return page-aligned address.
- `buddy_free(pa, order)` — merge with buddy if possible, repeat.
- `kalloc()` becomes `buddy_alloc(0)` — single-page allocation is
  just order 0. Existing code that calls `kalloc`/`kfree` works
  unchanged.
- New API: `kalloc_pages(order)` allocates 2^order contiguous pages
  via `buddy_alloc`. The caller computes the order.

At boot, `kalloc_init` groups all free memory into the largest
possible buddy blocks instead of adding pages one by one.

With the buddy allocator in place, the full allocator stack from
Part 1 is complete: slab on top of buddy on top of physical memory
— the same architecture as Linux, just much simpler.

---

## Part 8: How Linux Does It — SLUB (Simplified)

### Linux's allocator stack

Linux has a three-layer allocation stack, similar in spirit to what
we're building but far more sophisticated:

```
  ┌─────────────────────────────────────┐
  │  kmalloc() / kfree()                │  ← User-facing API
  │  Routes to size-class slab caches   │
  └──────────────┬──────────────────────┘
                 │
  ┌──────────────v──────────────────────┐
  │  SLUB allocator                     │  ← Slab layer
  │  Per-CPU freelists, partial lists   │
  │  Each "kmem_cache" = one type/size  │
  └──────────────┬──────────────────────┘
                 │
  ┌──────────────v──────────────────────┐
  │  Buddy allocator                    │  ← Page-level allocator
  │  Manages physical page frames       │
  │  Power-of-two coalescing            │
  └──────────────┬──────────────────────┘
                 │
  ┌──────────────v──────────────────────┐
  │  Physical memory (zones, NUMA)      │
  └─────────────────────────────────────┘
```

The middle layer, **SLUB** (the "Unqueued Slab Allocator," which
replaced the original SLAB and the simpler SLOB), is where the
interesting design lives. Here's a simplified view:

### kmem_cache

Each object type (or size class) gets a `struct kmem_cache`:

```c
struct kmem_cache {
    const char *name;       // "task_struct", "kmalloc-64", etc.
    int object_size;        // size of each object in bytes
    int size;               // object_size + alignment + metadata
    struct kmem_cache_cpu *cpu_slab;  // per-CPU freelists
    struct list_head partial;          // partially-full slabs
    ...
};
```

Linux creates two kinds of caches:

1. **Named caches** for specific types:
   `kmem_cache_create("task_struct", sizeof(struct task_struct), ...)`
   — these are created by subsystems that allocate many objects of
   one type.

2. **Generic caches** for `kmalloc` size classes:
   `kmalloc-32`, `kmalloc-64`, `kmalloc-128`, ..., `kmalloc-8192`.
   These are the same idea as our size classes.

### Per-CPU freelists

The key optimization in SLUB: each CPU core has its own freelist for
each cache. When CPU 0 calls `kmalloc(64)`, it pops from CPU 0's
freelist — no lock needed, no atomic operations, no contention with
other CPUs. This makes allocation incredibly fast on multiprocessor
systems.

When the per-CPU freelist runs out, SLUB moves to the **partial
list** — a list of slabs that have some free and some allocated
objects. It picks one and installs it as the new per-CPU slab.

When no partial slabs exist, SLUB asks the buddy allocator for a
new page (or pages) and initializes a fresh slab.

bobchouOS is single-hart, so we don't need per-CPU freelists. But
understanding why they exist helps explain the design of production
allocators. When we add multi-hart support, we could add per-hart
freelists to avoid lock contention.

### Object metadata in SLUB

SLUB stores the freelist pointer **inside** the free object — same
trick as our slab design. But SLUB adds a twist: it can optionally
randomize the freelist order (security hardening against heap
exploitation) and add "red zones" (guard bytes before and after each
object to detect buffer overflows). These are debug features enabled
by kernel config options.

> **Where does Linux store slab metadata?** Same place we do — in
> `struct page`. Modern SLUB stores slab metadata directly in
> `struct page` (more recently split into a dedicated `struct slab`
> that shares the same memory). Linux's `struct page` is a union of
> many different layouts depending on how the page is used — slab,
> page table, anonymous user page, file-backed page, etc. When a page
> is used as a slab, the `struct page` fields store the cache pointer,
> freelist, and object count. Our `struct page` union follows the same
> pattern, just with fewer variants so far.

---

## Part 9: bobchouOS Design Decisions

| Aspect | bobchouOS | Linux SLUB | Rationale for our choice |
|---|---|---|---|
| Page allocator | Buddy, orders 0-10 (4 KB – 4 MB) | Buddy with zones, NUMA, migration | Same algorithm, we skip NUMA/zones |
| Slab approach | Size-class pools | Size-class + named caches | Named caches not needed yet |
| Size classes | 32, 64, 128, …, 2048 | 8, 16, 32, …, 8192 | Power-of-two, covers typical kernel objects |
| Slab size | 1 page (4 KB) | 1-8 pages (configurable) | Simple — one `kalloc` per slab |
| Slab metadata | In `struct page` union | In `struct page` / `struct slab` | Same approach — full page usable for slots |
| Free list | Per-slab, embedded in slots | Per-CPU + partial list | Single hart — no per-CPU needed yet |
| Large allocs | Route to buddy allocator | Route to buddy allocator | Same |
| Empty slabs | Keep one per class, free extras | Cache many, free under memory pressure | Simpler fast path, no cold-start check |
| Locking | None | Per-CPU = lockless, partial = locked | Single hart for now |
| Debug features | Junk-fill on free | Red zones, poisoning, KASAN | Junk-fill catches use-after-free cheaply |

### Data structures

**`struct page` (updated from Phase 3):**

```c
struct page {
    uint16 refcount;
    uint8 order;              // buddy order (0 = single page)
    union {
        struct {              // when page is used as a slab
            uint8 class_idx;  // index into size_classes[] or BIG_ALLOC
            uint16 nr_alloc;  // currently allocated slots
            void *free_list;  // first free slot
            struct page *next_slab;  // next slab in class list
        } slab;
        // future: page table metadata, user page metadata, ...
    };
};
```

The `order` field sits outside the union because it's needed for
`kfree` regardless of how the page is used — buddy merging needs to
know the block size.

**Slab allocator structures (internal to `kmalloc.c`):**

```c
/* One per size class (7 classes for 32..2048). */
struct size_class {
    uint32 slot_size;         // 32, 64, ..., 2048
    struct page *slabs;       // linked list of slab pages (via page->slab.next_slab)
};

/* Free slot link — overlaid on free slot memory. */
struct free_slot {
    struct free_slot *next;
};
```

**Buddy allocator structures (internal to `kalloc.c`):**

```c
#define MAX_ORDER 10           // max block = 2^10 = 1024 pages = 4 MB

struct free_area {
    struct run *free_list;    // free blocks of this order
    uint64 nr_free;           // count of free blocks
};

static struct free_area free_areas[MAX_ORDER + 1];
```

### Size class selection

Given a requested size, we need to find the smallest size class that
fits. A simple loop does the job:

```c
static int
size_to_class(uint64 size) {
    for (int i = 0; i < NR_SIZE_CLASSES; i++)
        if (size <= size_classes[i].slot_size)
            return i;
    return -1;  // too large for slab — use big-alloc path
}
```

With only 7 classes over compile-time-constant values, the compiler
will unroll this into a short chain of compare-and-branch
instructions — effectively O(1).

> **Bit-trick alternative.** Since our classes are powers of two
> starting at 32 (= 2^5), you could compute the index directly:
> normalize the size past the 32-byte base with `(size - 1) >> 5`,
> then count how many bit positions remain via a shift loop
> (`while (size) { size >>= 1; idx++; }`) or a compiler builtin
> (`64 - __builtin_clzl(size - 1) - 5`). Both compute
> `ceil(log2(size / 32))` in O(1). In practice, the compiler will
> optimize our 7-iteration constant-bound loop into equally fast
> code — unrolled compares or a branch table — so the bit trick
> buys no real speedup and is harder to read.

---

## Part 10: Implementation Plan

### Files

```
kernel/
    include/
        kalloc.h          <-- UPDATE: expand struct page with union, add buddy API
        kmalloc.h         <-- NEW: kmalloc/kmfree declarations
    kalloc.c              <-- UPDATE: replace flat free list with buddy allocator
    kmalloc.c             <-- NEW: slab allocator (kmalloc, kmfree)
    main.c                <-- UPDATE: call kmalloc_init()
    test/
        test_kalloc.c     <-- UPDATE: add buddy allocator tests
        test_kmalloc.c    <-- NEW: slab allocator tests
        run_tests.c       <-- UPDATE: add test_kmalloc()
Makefile                  <-- UPDATE: add kmalloc.o and test_kmalloc.o
```

### `kalloc.h` — updated

```c
#define MAX_ORDER 10

struct page {
    uint16 refcount;
    uint8 order;              // buddy order (0 = single page)
    union {
        struct {
            uint8 class_idx;
            uint16 nr_alloc;
            void *free_list;
            struct page *next_slab;
        } slab;
    };
};

void kalloc_init(void);
void *kalloc(void);           // single page (order 0)
void kfree(void *pa);         // free single page
void *kalloc_pages(int order); // 2^order contiguous pages
void kfree_pages(void *pa, int order);
struct page *pa_to_page(uint64 pa);
```

`kalloc()` and `kfree()` remain unchanged in signature — they're
`buddy_alloc(0)` and `buddy_free(pa, 0)` under the hood. Existing
code (vm.c, etc.) doesn't need to change.

### `kmalloc.h`

```c
#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h"

void kmalloc_init(void);
void *kmalloc(uint64 size);
void kmfree(void *ptr);

#endif /* KMALLOC_H */
```

Minimal public interface. The slab internals (`struct size_class`,
`struct free_slot`) are private to `kmalloc.c`.

### `kalloc.c` — buddy allocator internals

**Key functions:**

**`kalloc_init()`** — Build the `struct page` array as before.
Then, instead of adding pages one at a time to a flat free list,
group contiguous free memory into the largest possible buddy blocks.
Starting from the first free page, find the largest power-of-two
block that fits at each aligned address, and add it to the
corresponding order's free list.

**`buddy_alloc(order)`** — Find a free block of the requested
order. If none available, split a larger block. Set `page->order`
on the allocated block.

**`buddy_free(pa, order)`** — Return a block to its order's free
list. Compute the buddy address (`pa XOR (PG_SIZE << order)`),
check if the buddy is free. If so, merge and repeat at the next
order.

**`kalloc()` / `kfree()`** — Thin wrappers: `buddy_alloc(0)` and
`buddy_free(pa, 0)`.

**`kalloc_pages(order)` / `kfree_pages(pa, order)`** — Direct
access to the buddy allocator for multi-page allocations.

### `kmalloc.c` — slab allocator internals

**`kmalloc_init()`** — Initialize the `size_classes` array and
pre-allocate one slab per class via `slab_create()`. This ensures
every class always has at least one slab, eliminating the "no slab
exists" check from the allocation fast path.

**`slab_create(class_idx)`** — Call `kalloc()` for a page. Set up
`struct page` slab fields (`class_idx`, `nr_alloc = 0`,
`free_list`). Divide the entire page into slots and build a free
list through them. Link into the class's slab list.

**`kmalloc(size)`**:

1. If `size == 0`, return NULL.
2. If `size > MAX_SLAB_SIZE`, big-alloc path: round up to
   power-of-two pages, `kalloc_pages(order)`, set
   `page->slab.class_idx = BIG_ALLOC`, return the pointer.
3. Find the size class via loop.
4. If the first slab has free slots, pop one.
5. Otherwise, `slab_create()`, prepend, pop.

**`kmfree(ptr)`**:

1. `pa_to_page(ptr)` → `struct page`.
2. If `class_idx == BIG_ALLOC`, call
   `kfree_pages(ptr, page->order)`.
3. Otherwise, push slot onto `page->slab.free_list`, decrement
   `nr_alloc`.
4. If `nr_alloc == 0` and the class already has another empty slab,
   unlink this one and `kfree()`. Otherwise keep it.

### Slab unlinking detail

When an empty slab needs to be freed, we must remove it from its
class's singly-linked list (via `page->slab.next_slab`). This
requires finding the previous slab — O(n) in the worst case. For
our teaching kernel with a small number of slabs, this is fine.

### Initialization ordering in kmain

```c
void
kmain(void) {
    uart_init();
    // ... trap setup, boot banner ...

    kalloc_init();            // buddy allocator — must be first
    vm_create_kernel_pt();    // kernel page table
    vm_enable_paging();       // enable Sv39

    kmalloc_init();           // slab allocator — after paging is on

    // ... tests, timer loop ...
}
```

`kalloc_init()` still comes first (everything else depends on it).
`kmalloc_init()` comes after paging is enabled — good practice for
forward compatibility if we later move the kernel to a non-identity-
mapped address space.

### Test plan

**Buddy allocator tests (in `test_kalloc.c`):**

1. Single page alloc/free (existing tests, still pass)
2. Multi-page allocation — `kalloc_pages(2)` (order 2 = 4 pages)
   returns a 16 KB-aligned contiguous block
3. Buddy merging — allocate and free two adjacent blocks, verify
   they merge
4. Splitting — allocate a small order when only large blocks are
   available

**Slab allocator tests (new `test_kmalloc.c`):**

1. Basic allocation and free — `kmalloc(N)` returns non-NULL,
   `kmfree` doesn't crash
2. Alignment — returned pointers are aligned to at least the
   size class
3. Size class routing — allocations of different sizes go to the
   right class
4. Reuse after free — freed slots are reused by subsequent
   allocations of the same class
5. Multiple slabs — exhaust one slab's slots, verify a second
   slab is allocated
6. Big allocation path — `kmalloc(3000)` and `kmalloc(8192)` take
   the big-alloc path (1 page and 2 pages) and can be freed
7. Zero-size allocation — `kmalloc(0)` returns NULL
8. Slab reclamation — freeing all objects when a class has two
   empty slabs returns one page to buddy allocator
9. One-slab-kept — freeing all objects in the only slab of a class
   keeps it alive (not freed)

### Expected boot output

```
bobchouOS is booting...
running in S-mode
sstatus=0x8000000200006022
kernel: 0x80000000 .. 0x8000XXXX (NNNNN bytes)
kalloc_init: buddy allocator, orders 0-10, NNNNN free pages (NNN MB)
vm_create_kernel_pt: page table at 0x8XXXXXXX
vm_enable_paging: Sv39 enabled
kmalloc_init: 7 size classes (32..2048), big alloc > 2048

timer interrupts enabled, waiting for ticks...
timer: 1 seconds
...
```

---

## Part 11: Looking Ahead

### What this round enables

With a buddy allocator and slab allocator, the kernel has a complete
memory management stack:

| Need | Allocator | Example |
|---|---|---|
| Sub-page object (≤ 2048 B) | `kmalloc` (slab) | `struct proc`, `struct file` |
| Single page (4 KB) | `kalloc` (buddy order 0) | Page tables, user pages |
| Multi-page contiguous (> 4 KB) | `kalloc_pages` (buddy order N) | DMA buffers, large kernel stacks |

Without `kmalloc`, each small object would consume a full page. With
1000 open files (64 bytes each), that's 4 MB wasted. With `kmalloc`
using the 64-byte class, those same 1000 files fit in ~16 pages
(64 KB).

### What we won't build (and why)

- **Named caches** (like Linux's `kmem_cache_create`) — our size
  classes cover everything we need. Named caches are valuable when
  you want custom constructors/destructors, NUMA awareness, or
  per-type statistics. We don't need any of that.

- **Per-hart freelists** — single hart for now. When multi-hart
  arrives, we can add this as an optimization.

- **Aggressive slab caching** — we keep at most one empty slab per
  class. Production allocators may cache several empty slabs or use
  per-CPU slab magazines for even faster reuse.

### What's next

After implementing the buddy allocator and `kmalloc`/`kmfree` and
passing all tests, Phase 4 (Virtual Memory) is complete. Phase 5
will introduce processes: a `struct proc`, a user page table,
context switching between kernel and user mode, and the first system
calls. `kmalloc` will be used heavily to allocate process-related
data structures.

---

## Quick Reference

### Allocation layers

```
  kmalloc(N)                any size — uniform interface
       |
       |  if N <= 2048:     pop from size-class slab
       |  if N > 2048:      big-alloc via buddy (round up to pages)
       v
  kalloc() / kalloc_pages() page-level allocation (buddy allocator)
       |
       |  orders 0-10:      1 page (4 KB) to 1024 pages (4 MB)
       |  split on alloc, merge on free
       v
  Physical memory            DRAM
```

### Size classes (slab allocator)

| Index | Size class | Slots per slab |
|---|---|---|
| 0 | 32 | 128 |
| 1 | 64 | 64 |
| 2 | 128 | 32 |
| 3 | 256 | 16 |
| 4 | 512 | 8 |
| 5 | 1024 | 4 |
| 6 | 2048 | 2 |

### Key formulas

```
  slots_per_slab = PG_SIZE / slot_size    (no header overhead)

  page_from_ptr  = pa_to_page((uint64)ptr)

  class_from_ptr = page_from_ptr->slab.class_idx

  buddy_addr     = addr XOR (PG_SIZE << order)
```

### API

| Function | Purpose |
|---|---|
| `kalloc()` | Allocate one 4 KB page (buddy order 0) |
| `kfree(pa)` | Free one page |
| `kalloc_pages(order)` | Allocate 2^order contiguous pages |
| `kfree_pages(pa, order)` | Free a multi-page block |
| `kmalloc_init()` | Initialize slab size class table |
| `kmalloc(size)` | Allocate `size` bytes (any size), returns pointer or NULL |
| `kmfree(ptr)` | Free memory returned by `kmalloc` |

### Data structures

```c
/* Per-page metadata (kalloc.h) — one per physical page. */
struct page {
    uint16 refcount;
    uint8 order;              // buddy order
    union {
        struct {              // slab metadata
            uint8 class_idx;
            uint16 nr_alloc;
            void *free_list;
            struct page *next_slab;
        } slab;
    };
};

/* Slab allocator internals (kmalloc.c) */
struct size_class {
    uint32 slot_size;         // 32, 64, ..., 2048
    struct page *slabs;       // linked list via page->slab.next_slab
};
```

### bobchouOS vs. xv6

| Aspect | xv6 | bobchouOS |
|---|---|---|
| Page allocator | Flat free list (single pages only) | Buddy allocator (orders 0-10) |
| Sub-page allocator | None | Slab allocator (7 size classes) |
| Slab metadata | — | External, in `struct page` union |
| Small object allocation | Full page per object | Packed into slab pages |
| Multi-page contiguous | Not supported | `kalloc_pages(order)` |
| API | kalloc/kfree only | kalloc/kfree + kalloc_pages + kmalloc/kmfree |
