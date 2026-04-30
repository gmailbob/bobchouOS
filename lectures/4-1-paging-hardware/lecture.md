# Lecture 4-1: Paging Hardware and Kernel Address Space

> **Where we are**
>
> Phase 3 is complete. The kernel has a working page allocator:
> `kalloc()` hands out zeroed 4 KB pages, `kfree()` returns them to the
> free list, and a `struct page` array tracks per-page metadata
> (refcount) for every physical page. The allocator boots, reports \~31K
> free pages (\~125 MB), and passes 26 tests.
>
> But there is no virtual memory. The `satp` register is still zero,
> which means MODE=Bare: every address in every `ld`/`sd` instruction
> *is* a physical address, sent straight to the memory bus. There is no
> isolation — if we had user processes, they could read the kernel's
> data, overwrite each other's memory, or poke device registers
> directly. There is no protection — a wild pointer in the kernel can
> stomp anything from UART registers to the page allocator's free list.
>
> Phase 4 enables virtual memory. The CPU will translate every memory
> access through a **page table** — a data structure that maps virtual
> addresses to physical addresses, one 4 KB page at a time. The
> hardware does the translation on every instruction, automatically.
> The kernel controls *what* is mapped and *how* (read-only? writable?
> executable? user-accessible?) by programming the page table entries.
>
> This first lecture covers the hardware side: how Sv39 page tables
> work, how the CPU walks them, and how the kernel address space is
> organized. The implementation plan at the end of this lecture
> describes the code we'll write: `vm.c` for page table creation,
> mapping, and enabling paging. A later round adds the kernel heap
> allocator (`kmalloc`).
>
> By the end of this lecture, you will understand:
>
> - Why virtual memory exists and what problems it solves
> - The Sv39 paging scheme: 39-bit virtual addresses, 56-bit physical
>   addresses, 3-level page tables
> - How a virtual address is split into VPN[2], VPN[1], VPN[0], and
>   page offset
> - The page table entry (PTE) format: PPN, flags (V, R, W, X, U, G,
>   A, D)
> - How the hardware walks the 3-level tree to translate an address
> - The `satp` CSR: MODE, ASID, PPN fields — how to enable paging
> - The TLB (Translation Lookaside Buffer) and `sfence.vma`
> - The kernel address space: direct mapping, device MMIO, the
>   trampoline (preview)
> - How xv6 organizes its kernel page table (`kvminit`, `kvmmap`,
>   `walk`, `mappages`)
> - bobchouOS design decisions vs. xv6
>
> **xv6 book coverage:** This lecture absorbs Chapter 3, sections 3.1
> (Paging hardware), 3.2 (Kernel address space), and 3.3 (Code:
> creating an address space). Sections 3.6–3.8 (Process address space,
> sbrk, exec) are previewed but deferred to Phase 6. Section 4.6
> (Page-fault exceptions) from Chapter 4 is previewed for context.

---

## Part 1: Why Virtual Memory?

### The problem

Right now, bobchouOS runs with paging disabled (`satp = 0`). Every
address the CPU encounters — in a `ld`, `sd`, `jal`, or instruction
fetch — goes straight to the physical address bus. This is fine for a
single kernel running alone, but it creates three problems the moment
we want to run user programs:

**1. No isolation.** If two programs are loaded into physical memory,
nothing stops program A from reading or writing program B's memory.
A buggy program can corrupt another program — or the kernel itself.

**2. No flexibility.** Each program must be compiled to run at a
specific physical address. If program A is loaded at `0x80100000` and
program B at `0x80200000`, their code must be linked for those exact
addresses. If a third program needs to go between them, and there
isn't enough contiguous space, you're stuck.

**3. No protection.** The kernel's code and data sit in the same
physical address space as everything else. A user program could jump
to the kernel's page allocator, or overwrite the trap vector, or
reprogram the UART.

### The solution: a layer of indirection

Virtual memory solves all three problems by adding a translation
layer between the CPU and physical memory:

```
  CPU issues address 0x1000
         |
         v
  +------------------+
  | Page Table Walk  |  (hardware, automatic)
  +------------------+
         |
         v
  Physical address 0x80205000
         |
         v
  DRAM / Device
```

The key idea: the address in the instruction (the **virtual address**)
is not the address that reaches memory (the **physical address**). A
data structure called the **page table** defines the mapping. The
kernel programs the page table; the hardware follows it.

This gives us:

- **Isolation:** each process gets its own page table, so process A's
  virtual address `0x1000` maps to a *different* physical page than
  process B's `0x1000`. They can't see each other's memory.

- **Flexibility:** every process can be linked to start at virtual
  address `0x0`. The page table maps those virtual addresses to
  whatever scattered physical pages `kalloc()` handed out. No
  contiguous physical memory required.

- **Protection:** each page table entry carries permission bits (read,
  write, execute, user-accessible). The kernel maps its own pages
  without the User bit — hardware will refuse access from U-mode.
  Code pages are mapped read+execute, not write — a buffer overflow
  can't modify instructions.

> **Historical note:** Virtual memory was invented in the late 1950s at
> the University of Manchester (Atlas computer, 1962). The original
> motivation was simpler than isolation — it was about letting programs
> use more memory than physically existed, by transparently swapping
> pages to disk. Isolation and protection came later as multi-user
> time-sharing systems (Multics, 1960s) needed to run untrusted
> programs side by side. Today, virtually every general-purpose CPU
> has a hardware page table walker — it's considered essential, not
> optional.

### Translation granularity: pages

The translation doesn't happen per-byte — that would require a table
entry for every byte of memory (billions of entries). Instead, it
happens per **page**: an aligned 4 KB (4096 byte) chunk of memory.

> **Is 4 KB universal?** Almost. x86, ARM, and RISC-V all default to
> 4 KB pages — it's the standard granularity across virtually every
> modern architecture. The one notable exception is ARM: ARMv8
> supports 16 KB and 64 KB as the *base* page size (not just as huge
> pages, but as the fundamental granularity). Apple's M-series chips
> use 16 KB base pages, which is why iOS/macOS developers occasionally
> hit alignment issues that don't exist on x86 or RISC-V.
>
> All three architectures also support **huge pages** — larger
> mappings that cover 2 MB or 1 GB in a single TLB entry. We'll see
> these as Sv39 "megapages" and "gigapages" in Part 4. The key
> tradeoff: huge pages reduce TLB pressure (one entry covers far more
> memory), but require contiguously aligned physical RAM, which gets
> harder to find as memory fragments over time.
>
> **The OS must match the hardware.** The kernel's `PG_SIZE` must
> equal the hardware's base page size — they are not independent
> choices. The page table format, the number of offset bits in a
> virtual address, PTE indexing, and alignment checks all depend on
> it. If the kernel thinks pages are 4 KB but the hardware walks
> tables assuming 16 KB, PTEs end up at wrong offsets and everything
> breaks. This is why ARM Linux has compile-time config for the base
> page size (`CONFIG_ARM64_4K_PAGES`, `CONFIG_ARM64_16K_PAGES`,
> `CONFIG_ARM64_64K_PAGES`) — the kernel is built for exactly one
> page size. For RISC-V, the spec hardcodes 4 KB for all Sv modes
> (Sv39, Sv48, Sv57), so our `PG_SIZE = 4096` is the only correct
> value.

We already defined `PG_SIZE = 4096` and `PG_SHIFT = 12` in Phase 3.
Now those constants gain a deeper meaning: they're the granularity
of address translation. One page table entry controls one 4 KB region.

The bottom 12 bits of a virtual address (the **page offset**) pass
through untranslated — they select a byte within the page. The upper
bits are the **virtual page number (VPN)**, which the hardware uses
to look up the page table entry that provides the **physical page
number (PPN)**. The final physical address is `PPN << 12 | offset`.

```
  Virtual Address:   [ VPN (upper bits) | Offset (12 bits) ]
                           |                    |
                     page table lookup     pass through
                           |                    |
                           v                    v
  Physical Address:  [ PPN (upper bits) | Offset (12 bits) ]
```

---

## Part 2: The Sv39 Paging Scheme

### RISC-V paging modes

RISC-V defines multiple paging schemes, selected by the MODE field
in the `satp` CSR:

| MODE value | Name | Virtual address bits | Page table levels | For |
|---|---|---|---|---|
| 0 | Bare | — | — | No translation (what we have now) |
| 8 | Sv39 | 39 | 3 | 512 GB virtual address space |
| 9 | Sv48 | 48 | 4 | 256 TB virtual address space |
| 10 | Sv57 | 57 | 5 | 128 PB virtual address space |

xv6 uses **Sv39** (**S**upervisor **v**irtual memory, **39**-bit
addresses — the name reflects that paging is controlled by S-mode via
the `satp` CSR). So does bobchouOS. It provides a 512 GB virtual
address space — far more than our 128 MB of physical RAM, and enough
for any reasonable kernel + user program layout.

Sv48 and Sv57 exist for systems with very large memory (servers,
data centers). They add more levels to the page table tree, which
means more memory overhead and slower worst-case walks. For a
teaching OS, Sv39 is the right choice.

> **Why not Sv32?** Sv32 is for 32-bit RISC-V (RV32). It uses 2-level
> page tables with 32-bit PTEs, giving a 4 GB virtual address space.
> We're building for RV64, so Sv32 isn't available. (Sv39 is the
> minimum for RV64.)

### Sv39 address layout: virtual and physical side by side

An Sv39 virtual address is 39 bits wide (in a 64-bit register); the
physical address produced by translation is 56 bits wide. Here they
are side by side — notice how the page offset passes through
unchanged, while the VPN fields are *replaced* by PPN fields:

```
  Virtual address (in CPU register, 64 bits)

  63      39 38      30 29      21 20      12 11            0
  +---------+----------+----------+----------+--------------+
  |   EXT   |  VPN[2]  |  VPN[1]  |  VPN[0]  |  page offset |
  +---------+----------+----------+----------+--------------+
    25 bits     9 bits     9 bits     9 bits      12 bits
                ~~~~~~     ~~~~~~     ~~~~~~
                index      index      index      pass through
                into L2    into L1    into L0    unchanged
                  |           |          |             |
  63   56 55      v           v          v             v    0
  +------+-------------+----------+----------+--------------+
  | 0..0 |    PPN[2]   |  PPN[1]  |  PPN[0]  |  page offset |
  +------+-------------+----------+----------+--------------+
   8 bits     26 bits     9 bits     9 bits      12 bits
   unused

  Physical address (on memory bus, 56 bits used)
```

**Virtual address fields:**

- **Bits 11:0** — **Page offset** (12 bits). Selects a byte within the
  4 KB page. Passes through translation unchanged.

- **Bits 20:12** — **VPN[0]** (9 bits). Index into the level-0 (leaf)
  page table. Range: 0–511.

- **Bits 29:21** — **VPN[1]** (9 bits). Index into the level-1 (middle)
  page table. Range: 0–511.

- **Bits 38:30** — **VPN[2]** (9 bits). Index into the level-2 (root)
  page table. Range: 0–511.

- **Bits 63:39** — **Extension** (25 bits). Must be copies of bit 38
  (sign extension). This is a hardware requirement — if these bits
  don't match bit 38, the address is invalid and causes a page fault.

**Physical address fields:**

- **Bits 11:0** — **Page offset** (12 bits). Same as virtual — copied
  directly.

- **Bits 20:12** — **PPN[0]** (9 bits). From the leaf PTE.

- **Bits 29:21** — **PPN[1]** (9 bits). From the leaf PTE.

- **Bits 55:30** — **PPN[2]** (26 bits). From the leaf PTE. Note this
  is **26 bits**, not 9 like VPN[2]. The PTE format is shared across
  all Sv modes (Sv39/48/57), and the 44-bit PPN field must be wide
  enough to address any physical page in the 56-bit physical address
  space. It's not about physical being larger than virtual — it's
  about the PTE being a fixed format.

Why 9 bits per level? The number is forced by two fixed quantities:
a page is 2^12 bytes and a PTE is 2^3 bytes (8 bytes), so one page
holds exactly 2^12 / 2^3 = 2^9 = 512 PTEs. You need 9 bits to index
one of 512 entries — that's the VPN width per level. Each page table
node is exactly one 4 KB page, exclusively filled with PTEs, no
wasted space. This is elegant: the page allocator can hand out page
table pages the same way it hands out any other page.

### The sign extension rule

In Sv39, only 39 bits of the virtual address are meaningful — but
RISC-V registers are 64 bits wide. What should bits 63:39 contain?
The hardware requires them to be copies of bit 38. If bit 38 is 0,
bits 63:39 must all be 0. If bit 38 is 1, bits 63:39 must all be 1.
Any other pattern causes an immediate page fault.

This copying rule is called **sign extension** — the same operation
we saw in Lecture 0-2 (assembly recap) for load instructions: `lb`
sign-extends a byte to 64 bits by copying the sign bit into the
upper bits, preserving the numeric value. Here the idea is identical
— bit 38 is "extended" into bits 63:39.

**Why enforce this?** Forward compatibility. Without this rule,
software might use bits 63:39 to store arbitrary data (pointer tags,
flags, etc.). Later, when the system upgrades to Sv48 (which
interprets 48 bits of the virtual address), those random upper bits
would suddenly be treated as part of the address — breaking
everything. By requiring sign extension, the spec guarantees that
every valid Sv39 address is *also* a valid Sv48 address. Software
written for Sv39 works on Sv48 without modification.

Consider a kernel address like `0xFFFF_FFC0_8000_0000`. Under Sv39,
bits 38:0 are the meaningful part, and bits 63:39 are all 1s — valid
sign extension of bit 38 (which is 1). Now switch to Sv48, where
bits 47:0 are meaningful. Bits 63:48 must be copies of bit 47 — and
they are, because they're all 1s and bit 47 is also 1. The address
passes the sign extension check under both modes — it won't fault
on format alone. (The actual *translation* will differ, because Sv48
interprets bits 47:39 as VPN[3], a new index level — so the OS must
set up Sv48 page tables to produce the right mappings. The guarantee
is that the address *format* is compatible — no pointers need to be
rewritten.)

The practical effect is that the 64-bit space splits into two usable
halves with a giant hole in the middle:

```
  0x0000_0000_0000_0000  ┌─────────────────────┐
                         │                     │  Lower half
                         │  User space         │  (bit 38 = 0, ext = 0)
                         │  0 .. 2^38 - 1      │  256 GB
  0x0000_003F_FFFF_FFFF  └─────────────────────┘
  0x0000_0040_0000_0000  ┌─────────────────────┐
                         │                     │
                         │     (invalid)       │  "Hole" in the middle
                         │                     │
  0xFFFF_FFBF_FFFF_FFFF  └─────────────────────┘
  0xFFFF_FFC0_0000_0000  ┌─────────────────────┐
                         │                     │  Upper half
                         │  Kernel space       │  (bit 38 = 1, ext = all 1s)
                         │  -2^38 .. -1        │  256 GB
  0xFFFF_FFFF_FFFF_FFFF  └─────────────────────┘
```

This is the same user/kernel split that x86-64 uses (with a
different number of bits). Addresses in the "hole" are non-canonical
and cause a page fault — a convenient guard between user and kernel
space.

> **Convention vs. requirement:** The hardware *requires* sign
> extension (it faults on non-canonical addresses), but the
> user/kernel split is a *convention*. You could put kernel mappings
> in the lower half. But using the upper half (high addresses) is
> universal across operating systems because it makes the split
> trivial: the top bit of the virtual address tells you whether it's
> a kernel address or a user address.

---

## Part 3: Page Table Entries (PTEs)

### PTE format

Each page table entry is a 64-bit value:

```
  63:54    53:28     27:19    18:10    9:8  7 6 5 4 3 2 1 0
  +--------+---------+--------+--------+---+-+-+-+-+-+-+-+-+
  |Reserved| PPN[2]  | PPN[1] | PPN[0] |RSW|D|A|G|U|X|W|R|V|
  |  (0)   | 26 bits | 9 bits | 9 bits | 2 |1|1|1|1|1|1|1|1|
  +--------+---------+--------+--------+---+-+-+-+-+-+-+-+-+
  10 bits       44-bit PPN              flags (10 bits)
```

```
  Bit(s)   Field       Meaning
  ------   -----       -------
  0        V           Valid — PTE is active
  1        R           Readable
  2        W           Writable
  3        X           Executable
  4        U           User-mode accessible
  5        G           Global mapping
  6        A           Accessed (set by hardware)
  7        D           Dirty (set by hardware on writes)
  9:8      RSW         Reserved for Software (kernel can use freely)
  53:10    PPN         Physical Page Number (44 bits)
  63:54    Reserved    Must be zero (future extensions)
```

The 44-bit PPN field contains the physical page number. To get the
physical address of the mapped page: `PPN << 12`. Combined with the
12-bit page offset from the virtual address, this gives the 56-bit
physical address.

### Flag bits in detail

**V (Valid) — bit 0.** If V=0, the PTE is unused. Any access to this
virtual address raises a page-fault exception (scause = 12, 13, or
15 depending on whether it's an instruction fetch, load, or store).
This is how the kernel marks regions of the address space as "not
mapped" — just leave V=0.

**R, W, X (Read, Write, Execute) — bits 1, 2, 3.** These control what
the processor is allowed to do with the mapped page:

| X | W | R | Meaning |
|---|---|---|---------|
| 0 | 0 | 0 | Not a leaf PTE — pointer to next-level page table |
| 0 | 0 | 1 | Read-only page |
| 0 | 1 | 0 | *Reserved for future use* |
| 0 | 1 | 1 | Read-write page |
| 1 | 0 | 0 | Execute-only page |
| 1 | 0 | 1 | Read-execute page |
| 1 | 1 | 0 | *Reserved for future use* |
| 1 | 1 | 1 | Read-write-execute page |

The critical row is the first one: when R=W=X=0, the PTE is **not a
leaf**. The PPN field points to the *next level* of the page table
tree, not to a final data/code page. This is how the hardware knows
whether to keep descending the tree or to stop and return a physical
address. In summary:

```
  Non-leaf (pointer):    V=1, R=0, W=0, X=0  →  PPN points to next table
  Leaf (final mapping):  V=1, R|W|X != 0     →  PPN is the physical page
  Invalid:               V=0                 →  page fault
```

> **Write implies Readable:** The spec requires that writable pages
> must also be readable (W=1 requires R=1). The combination R=0,
> W=1 is reserved.

**U (User) — bit 4.** If U=1, user-mode code can access this page. If
U=0, only supervisor mode can access it. This is the primary
mechanism for kernel/user isolation.

There's a subtle interaction with the SUM (Supervisor User Memory
access) bit in `sstatus`. By default (SUM=0), the supervisor *cannot*
access pages marked U=1. This prevents the kernel from accidentally
dereferencing a user pointer. When the kernel intentionally needs to
copy data to/from user memory (e.g., in a `write()` syscall), it
temporarily sets SUM=1. xv6 handles this with `copyin`/`copyout`
functions. bobchouOS will deal with this in Phase 6.

> **Why block the supervisor from user pages?** It sounds backwards —
> shouldn't the kernel be able to access everything? The protection is
> deliberate: if the kernel blindly follows a pointer from user space,
> an attacker could trick the kernel into reading or writing
> kernel-only memory by passing a kernel address as a "user pointer."
> This is called a **confused deputy** attack. Blocking supervisor
> access to U=1 pages by default forces the kernel to explicitly opt
> in when crossing the user/kernel boundary.

**G (Global) — bit 5.** Marks a mapping as global — it exists in all
address spaces. When the CPU switches page tables (e.g., during a
context switch), TLB entries marked Global are not flushed. Kernel
mappings are the obvious candidate: they're the same in every
process's page table, so flushing and re-walking them on every
context switch wastes time.

**A (Accessed) — bit 6.** The hardware sets this bit when the page is
read, written, or executed. The OS can clear it periodically and
check which pages were accessed since the last clear — this is how
page replacement algorithms (LRU, clock) identify "hot" vs. "cold"
pages. We won't use this in bobchouOS (no swapping), but it's good
to understand.

**D (Dirty) — bit 7.** The hardware sets this bit when the page is
written. If a page is clean (D=0), the OS knows it doesn't need to
write it back to disk before reclaiming it. Again, we won't use this
until swap is implemented (if ever).

> **A/D bit implementation choices:** The RISC-V spec allows two
> approaches. The hardware can either (1) set A/D bits automatically
> (like x86), or (2) raise a page fault when A=0 or D=0, letting the
> OS set them in software. QEMU uses approach (1). Real hardware
> varies. For simplicity, bobchouOS will pre-set A and D bits to 1 in
> all PTEs, avoiding page faults from unset access/dirty bits.

**RSW (Reserved for Software) — bits 9:8.** The hardware ignores these
bits. The kernel can use them for any purpose — some kernels store
COW flags or other metadata here. xv6 doesn't use them. We might use
one for the COW bit in Phase 6.

---

## Part 4: The Three-Level Page Table Walk

### How it works

When the CPU needs to translate a virtual address (on every
instruction fetch, load, and store), it performs the following
steps:

```
  satp register          Virtual Address
  +-------------+        +--------+--------+--------+--------+
  |MODE|ASID|PPN|        | VPN[2] | VPN[1] | VPN[0] | offset |
  +----+----+---+        | 9 bits | 9 bits | 9 bits | 12 bits|
        |                +--------+--------+--------+--------+
        |                    |        |        |         |
        v                    |        |        |         |
  +-----------+              |        |        |         |
  | L2 table  |<-- PPN<<12   |        |        |         |
  |  (root)   |              |        |        |         |
  |           |              |        |        |         |
  | entry[VPN[2]]----------->|        |        |         |
  +-----------+              |        |        |         |
        |                    |        |        |         |
        | PPN from L2 PTE    |        |        |         |
        v                    |        |        |         |
  +-----------+              |        |        |         |
  | L1 table  |              |        |        |         |
  |  (middle) |              |        |        |         |
  |           |              |        |        |         |
  | entry[VPN[1]]----------->+        |        |         |
  +-----------+                       |        |         |
        |                             |        |         |
        | PPN from L1 PTE             |        |         |
        v                             |        |         |
  +-----------+                       |        |         |
  | L0 table  |                       |        |         |
  |  (leaf)   |                       |        |         |
  |           |                       |        |         |
  | entry[VPN[0]]-------------------->+        |         |
  +-----------+                                |         |
        |                                      |         |
        | PPN from L0 PTE (leaf)               |         |
        v                                      v         |
  +----------------------------------------------------+ |
  | Physical address = (leaf PPN << 12) | offset       | |
  +----------------------------------------------------+-+
```

Step by step:

1. **Read the root table address from `satp`.** `satp` stores a page
   number, not a byte address (the bottom 12 bits of a page-aligned
   address are always zero, so there's no point storing them).
   The physical address of the root (level-2) page table is
   `satp.PPN << 12` (equivalently, `satp.PPN * 4096`).

2. **Index into the L2 (root) table using VPN[2].** Read the PTE at
   address `(satp.PPN << 12) + (VPN[2] * 8)`. Each PTE is 8 bytes,
   and VPN[2] is 9 bits (0–511), so `VPN[2] * 8` ranges from 0 to
   4088 — always within the single 4096-byte page that `satp.PPN`
   points to. This is the same "one page = 512 PTEs" property from
   earlier: no matter what VPN[2] is, the access stays inside the
   root page table page. The same holds at every level — each 9-bit
   index always lands within its page table node.

3. **Check the PTE.** If V=0, page fault. If it's a leaf
   (R|W|X != 0), the walk is done — this is a **gigapage** (1 GB
   mapping). The physical address is `(PPN[2] << 30) | VA[29:0]`:
   bits 55:30 come from PPN[2] in the PTE, bits 29:0 come straight
   from the virtual address (VPN[1] + VPN[0] + offset = 30 bits =
   1 GB). The PTE's PPN[1] and PPN[0] must be zero, otherwise the
   hardware raises a misaligned superpage fault. Otherwise (R=W=X=0),
   it's a non-leaf pointer to the level-1 table.

4. **Index into the L1 (middle) table using VPN[1].** Extract the PPN
   from the L2 PTE, compute the L1 table address
   (`L2_PTE.PPN << 12`), read the PTE at offset `VPN[1] * 8`.

5. **Check the PTE.** If V=0, page fault. If it's a leaf, the walk is
   done — this is a **megapage** (2 MB mapping). The physical address
   is `(PPN[2:1] << 21) | VA[20:0]`: bits 55:21 come from PPN[2]
   and PPN[1] in the PTE, bits 20:0 come from the virtual address
   (VPN[0] + offset = 21 bits = 2 MB). The PTE's PPN[0] must be
   zero (misaligned superpage fault otherwise). Otherwise, it's a
   pointer to the level-0 table.

6. **Index into the L0 (leaf) table using VPN[0].** Extract the PPN
   from the L1 PTE, compute the L0 table address, read the PTE at
   offset `VPN[0] * 8`.

7. **Check the PTE.** If V=0, page fault. If it's a leaf, the walk is
   done — the PPN from this PTE is the physical page number for the
   final translation. If R=W=X=0 at level 0, it's also a page
   fault (there's no level below to descend to).

8. **Form the physical address:** `(leaf_PTE.PPN << 12) | offset`.

### A concrete example

Let's trace a full 3-level walk for virtual address `0x80001234`.

**Decompose the virtual address:**

```
  VA = 0x0000_0000_8000_1234

  Bits 38:0 in binary, grouped by field:

  VPN[2]      VPN[1]      VPN[0]      offset
  000_000_010 000_000_000 000_000_001 0010_0011_0100
  = 2         = 0         = 1         = 0x234
```

**Setup:** `satp.PPN = 0x80800`, so the root page table is at
physical address `0x80800 << 12 = 0x8080_0000`.

**Level 2 (root).** Index with VPN[2] = 2:
  - Read PTE at `0x8080_0000 + 2 × 8 = 0x8080_0010`
  - PTE has V=1, R=0, W=0, X=0 → non-leaf (pointer to L1 table)
  - PTE's PPN = `0x80801` → L1 table is at `0x8080_1000`

**Level 1 (middle).** Index with VPN[1] = 0:
  - Read PTE at `0x8080_1000 + 0 × 8 = 0x8080_1000`
  - PTE has V=1, R=0, W=0, X=0 → non-leaf (pointer to L0 table)
  - PTE's PPN = `0x80802` → L0 table is at `0x8080_2000`

**Level 0 (leaf).** Index with VPN[0] = 1:
  - Read PTE at `0x8080_2000 + 1 × 8 = 0x8080_2008`
  - PTE has V=1, R=1, W=1, X=0 → leaf (read-write data page)
  - PTE's PPN = `0x80205`

**Form the physical address:** `(0x80205 << 12) | 0x234 = 0x8020_5234`

```
  Summary:
  VA  0x0000_0000_8000_1234
       → L2[2] → L1[0] → L0[1] → PPN 0x80205
  PA  0x0000_0000_8020_5234
```

### The translation spectrum

The page table walk is not always 3 levels deep. Where the walk
*stops* depends on whether the PTE it reads is a leaf (R|W|X != 0)
or a pointer (R=W=X=0). The kernel controls this per-mapping by how
it programs the PTEs. You can think of the whole scheme as a
spectrum — from no translation to full 3-level walks:

| Mode | PTE lookups | Bits from PTE | Bits passed through from VA | Page size |
|---|---|---|---|---|
| Bare (satp=0) | 0 | 0 | all 64 → PA directly | — |
| Gigapage (leaf at L2) | 1 | PPN[2] (bits 55:30) | VA[29:0] = 30 bits | 1 GB |
| Megapage (leaf at L1) | 2 | PPN[2:1] (bits 55:21) | VA[20:0] = 21 bits | 2 MB |
| Regular page (leaf at L0) | 3 | PPN[2:0] (bits 55:12) | VA[11:0] = 12 bits | 4 KB |

Each level of lookup "peels off" 9 bits from the pass-through
portion and replaces them with bits looked up from a PTE. Bare mode
is the degenerate case: zero lookups, everything passes through,
VA = PA. A regular 4 KB page is the other extreme: 3 lookups, only
the 12-bit offset passes through.

Within a single Sv39 page table, different virtual address ranges can
use different page sizes. The `satp.MODE` field (Sv39 vs Sv48 vs
Sv57) determines the *maximum* depth — for Sv39, every walk starts
at level 2. But the walk stops early whenever it hits a leaf.

### Superpages: tradeoffs

Gigapages and megapages (collectively called **superpages**) have
clear advantages:

**Less memory for page tables.** A gigapage covers 1 GB with a single
PTE. Covering the same range with 4 KB pages requires hundreds of
page table pages: 1 L2 page + up to 512 L1 pages + up to 512×512 L0
pages. That's real memory overhead.

**Better TLB efficiency.** One gigapage = one TLB entry covering 1 GB.
With 4 KB pages, covering 1 GB requires 262,144 TLB entries —
impossible, since TLBs typically have 32–1024 entries. With small
pages, you get constant TLB misses and constant 3-level walks.

**Fewer memory accesses per walk.** A gigapage needs 1 PTE read, a
megapage needs 2, a regular page needs 3. Fewer reads = faster
worst-case translation.

But the disadvantage is significant — **coarse granularity**:

**One PTE = one set of permissions for the entire region.** You can't
make part of a gigapage read-only and another part read-write. If
your kernel's `.text` (read-execute) and `.data` (read-write) both
fall within the same 1 GB gigapage, you'd have to make the whole
thing RWX — losing the protection that caught accidental writes to
code.

**Must be backed by contiguous, aligned physical memory.** A 2 MB
megapage requires 2 MB of contiguous, 2 MB-aligned physical RAM. A
1 GB gigapage requires 1 GB contiguous, 1 GB-aligned. This is easy
at boot (memory hasn't fragmented yet) but gets harder at runtime as
pages are allocated and freed in scattered order.

**No fine-grained control.** You can't unmap a single 4 KB page in
the middle of a superpage (e.g., a guard page for stack overflow
detection). You can't do COW fork at 4 KB granularity within a
superpage. You can't map scattered physical pages into what looks
like a contiguous virtual range.

This is why user memory always uses 4 KB pages — you need per-page
control over permissions, mapping, and physical backing. Production
kernels use superpages mainly for the kernel's identity map (big,
contiguous, uniform permissions) and for large anonymous memory
regions where TLB pressure matters (databases, VMs).

> **Linux and superpages:** Linux calls 2 MB pages "huge pages" (or
> "transparent huge pages" when the kernel allocates them
> automatically). They're heavily used for database workloads and
> virtualization where TLB misses are a significant performance
> bottleneck. The kernel's identity mapping of physical memory
> typically uses 1 GB gigapages.

bobchouOS uses 4 KB pages for everything — simpler code, and the
performance difference is negligible on QEMU. But it's good to
understand the mechanism and tradeoffs, since the hardware supports
it and production kernels rely on it heavily.

### Cost of the walk

In the worst case (4 KB pages), the hardware reads **three** memory
locations to translate one virtual address (L2, L1, L0 PTEs). That's
three memory accesses before the actual data access — a 4x slowdown
if done naively. This is where the TLB comes in (Part 5).

---

## Part 5: The `satp` CSR

### Layout

The `satp` (Supervisor Address Translation and Protection) register
tells the CPU which page table to use and what translation mode is
active:

```
  63   60 59      44 43                         0
  +------+----------+---------------------------+
  | MODE |   ASID   |           PPN             |
  +------+----------+---------------------------+
  4 bits   16 bits             44 bits
```

- **MODE** (bits 63:60): Selects the translation scheme.
  - `0` = Bare (no translation — what we have now)
  - `8` = Sv39
  - `9` = Sv48
  - `10` = Sv57

- **ASID** (bits 59:44): Address Space IDentifier. A 16-bit tag that
  the hardware uses to avoid flushing the TLB on context switches.
  Without ASIDs, switching from process A to process B requires
  flushing the entire TLB (because A's cached translations would give
  wrong answers for B). With ASIDs, each TLB entry is tagged — the
  hardware only matches entries whose ASID equals `satp.ASID`, so A's
  and B's translations coexist in the TLB simultaneously. Switching
  processes just writes a new `satp` (new ASID + new PPN), no flush
  needed. xv6 doesn't use ASIDs (sets ASID=0, flushes TLB on every
  switch). bobchouOS won't either — it's a performance optimization,
  not a correctness requirement.

- **PPN** (bits 43:0): Physical Page Number of the root page table.
  The physical address of the root table is `PPN << 12`.

### Enabling Sv39

To enable paging, the kernel writes `satp` with MODE=8 and the PPN
of the root page table:

```c
#define SATP_SV39  (8UL << 60)

#define MAKE_SATP(pagetable) (SATP_SV39 | ((uint64)(pagetable) >> 12))

// Enable paging:
csrw(satp, MAKE_SATP(kernel_pagetable));
sfence_vma();   // flush TLB — critical!
```

After this write, *every* memory access goes through the page table.
The very next instruction fetch will be translated. This means the
page table must already contain a valid mapping for the address the
CPU is currently executing from — otherwise you get an immediate page
fault and crash.

> **The chicken-and-egg problem:** You're running at physical address
> `0x8000_XXXX`. You write `satp` to enable paging. The next
> instruction fetch uses the *virtual* address in `pc` — which is
> still `0x8000_XXXX`. If the page table doesn't map virtual
> `0x8000_XXXX` → physical `0x8000_XXXX`, you crash instantly.
>
> The solution is **identity mapping** (also called **direct
> mapping**): map the virtual address range `0x8000_0000` –
> `0x8800_0000` to the same physical addresses. After enabling
> paging, the kernel's code keeps working because `pc` translates
> to the same physical address as before. This is exactly what xv6
> (and bobchouOS) does.

### Changing `satp` safely

The spec does **not** guarantee that writing `satp` invalidates TLB
entries. Stale translations from the old page table (or from Bare
mode) may persist in the TLB after the write. Always issue
`sfence.vma` after writing `satp` — it's cheap (single instruction)
and eliminates an entire class of bugs.

---

## Part 6: The TLB and `sfence.vma`

### What is the TLB?

The Translation Lookaside Buffer is a hardware cache that stores
recently used virtual-to-physical translations. Without it, every
memory access would require three extra memory reads (the page table
walk). With it, the common case is a single-cycle lookup:

```
  CPU issues virtual address
         |
    +----v----+
    |   TLB   | ← small, fast cache (typically 32-1024 entries)
    +---------+
     |       |
    hit     miss
     |       |
     v       v
  physical  page table    ← 3 memory reads, slow
  address   walk
             |
             v
           physical
           address
           (also stored
            in TLB)
```

TLB hit rates are typically 99%+ for most workloads. The 3-level
walk only happens on misses.

### How this is implemented in hardware

Both the TLB and the page table walker are hardware circuits inside
the CPU — collectively called the **MMU (Memory Management Unit)**.
The kernel never runs code to translate addresses during normal
execution. The kernel's only job is to set up the page tables in
memory and write `satp`; the MMU does the rest automatically.

**The TLB** is a small piece of SRAM — the same fast memory
technology used for L1 cache. It's an associative lookup table: given
a virtual page number (and ASID), it returns the PPN and permission
bits, typically in 1 cycle. Think of it as a hardware hash map with
32–1024 entries.

**The page table walker** is a state machine (sequential logic) that
activates on TLB misses. It reads `satp` to get the root PPN,
computes each PTE address, and issues memory reads — one per level.
These reads go through the normal cache hierarchy (L1/L2/L3), so the
cost depends on whether the page table pages are cached:

| Scenario | Approximate cost |
|---|---|
| TLB hit | ~1 cycle |
| TLB miss, PTEs in L1 cache | ~10–15 cycles |
| TLB miss, PTEs in L2/L3 cache | ~30–50 cycles |
| TLB miss, PTEs in DRAM (cold) | ~200+ cycles |

Once the walker finds the leaf PTE, it installs the translation in
the TLB so subsequent accesses to the same page are fast.

> **Software vs. hardware page table walking.** Most modern
> architectures (x86, ARM, RISC-V by default) use hardware walkers.
> But some (notably MIPS, and optionally RISC-V) support **software
> TLB refill**: on a TLB miss, the hardware raises an exception and
> the OS runs a handler that walks the page table in software and
> inserts the entry into the TLB manually. This is simpler hardware
> but slower on misses. QEMU's RISC-V implementation uses a hardware
> walker.
>
> The `walk()` function we'll write in `vm.c` is the *software*
> equivalent of the hardware walker — but we use it to *build and
> modify* page tables, not to perform runtime translation. It's the
> same algorithm (descend 3 levels, follow PPN pointers), just
> running in C instead of in a state machine.

### The stale TLB problem

The TLB is a *cache*, and like all caches, it can become stale. If
the kernel modifies a page table entry (changes permissions, unmaps a
page, changes the physical page), the TLB might still hold the old
translation. The CPU would use the old mapping, leading to:

- Accessing a page that was unmapped (security hole)
- Writing to a page that was made read-only (protection violation
  missed)
- Accessing the wrong physical page entirely

### `sfence.vma` — TLB flush

The RISC-V instruction `sfence.vma` (Supervisor Fence, Virtual
Memory Access) flushes TLB entries. It has two register operands
that control the scope of the flush:

```
  sfence.vma  zero, zero    // flush ALL TLB entries (nuclear option)
  sfence.vma  va,   zero    // flush entries for virtual address va
  sfence.vma  zero, asid    // flush entries for a specific ASID
  sfence.vma  va,   asid    // flush entries for va in a specific ASID
```

xv6 always uses `sfence.vma zero, zero` (flush everything). This is
correct but potentially expensive on multiprocessor systems. For
bobchouOS on a single hart, it's the simplest and safest choice.

In C, we'll wrap it:

```c
static inline void
sfence_vma(void) {
    asm volatile("sfence.vma zero, zero");
}
```

> **When to issue `sfence.vma`:**
> - After writing `satp` (switching page tables)
> - After modifying any PTE (changing permissions, unmapping, etc.)
> - After creating new mappings (technically only needed if the TLB
>   might have cached an "invalid" entry for that address, but it's
>   safest to always flush)
>
> **When you can skip it:**
> - When building a page table that hasn't been installed yet (no
>   one is using it, so nothing is cached)
> - Before first enabling paging (the TLB is empty/irrelevant in
>   Bare mode)

### Multi-hart TLB considerations (preview)

On a multi-hart system, `sfence.vma` only flushes the *local* hart's
TLB. If another hart is using the same page table, its TLB is still
stale. The solution is a **TLB shootdown**: send an IPI
(inter-processor interrupt) to each other hart, have it execute
`sfence.vma` in its interrupt handler, and wait for acknowledgment.
bobchouOS is single-hart for now, so we don't need this. We'll
revisit TLB shootdowns when we add multi-hart support.

---

## Part 7: The Kernel Address Space

### Why the kernel needs a page table

After enabling paging, *all* memory accesses go through translation —
including the kernel's own code and data access. The kernel must have
its own page table that maps everything it needs:

1. **Kernel code and data** (`.text`, `.rodata`, `.data`, `.bss`,
   stack) — the kernel image itself
2. **Free memory** — pages managed by `kalloc()`, used for page
   tables, process memory, etc.
3. **MMIO devices** — UART (`0x10000000`), PLIC (`0x0C000000`) — the
   kernel must still be able to read/write device registers

### xv6's kernel address space

xv6 uses a **direct mapping** (also called **identity mapping**) for
the kernel: virtual address == physical address for most of the
address space. Here's the layout from Figure 3.3 of the xv6 book:

```
  Virtual Address          Physical Address        Permissions
  ─────────────────────────────────────────────────────────────
  MAXVA (top)
    Trampoline page  ───→  (physical trampoline)   R-X
    Guard page             (unmapped)              ---
    Kstack 0         ───→  (allocated page)        RW-
    Guard page             (unmapped)              ---
    Kstack 1         ───→  (allocated page)        RW-
    ...
  PHYSTOP (0x88000000)
    Free memory      ───→  0x88000000              RW-  (identity)
      ...                   ...
    Kernel data      ───→  (same PA)               RW-  (identity)
    Kernel text      ───→  (same PA)               R-X  (identity)
  KERNBASE (0x80000000)
    ...
  0x10001000
    VIRTIO disk      ───→  0x10001000              RW-  (identity)
  0x10000000
    UART0            ───→  0x10000000              RW-  (identity)
  0x0C000000
    PLIC             ───→  0x0C000000              RW-  (identity)
  ─────────────────────────────────────────────────────────────
```

Most mappings are identity maps: virtual `0x80001000` → physical
`0x80001000`. This makes kernel code simple — a physical address from
`kalloc()` is also a valid virtual address that the kernel can
dereference directly.

There are two exceptions to pure identity mapping:

1. **The trampoline page** — mapped at the very top of the virtual
   address space (just below MAXVA). This is a single page of code
   used for user/kernel transitions. It's mapped at a high virtual
   address so it appears at the same address in *every* process's
   page table (user and kernel). We'll implement this in Phase 6 when
   we add user processes.

2. **Kernel stacks** — each process gets a kernel stack mapped at a
   high virtual address with a guard page (unmapped page) below it.
   The guard page catches stack overflows: if the stack grows too much
   and hits the guard page, the hardware generates a page fault
   instead of silently corrupting memory. Without virtual memory,
   you'd have to waste a full physical page for each guard region.
   With virtual memory, you just leave a PTE invalid — free.

### bobchouOS kernel address space (Phase 4)

For this phase, bobchouOS uses a simpler layout — pure identity
mapping for everything:

```
  Virtual Address          Physical Address        Permissions
  ─────────────────────────────────────────────────────────────
  0x88000000 (PHYS_STOP)
    Free memory      ───→  same PA              ┐
    struct page array───→  same PA              │ RW-
    Kernel BSS+stack ───→  same PA              │ (one mapping call,
    Kernel data      ───→  same PA              │  _text_end to PHYS_STOP)
    Kernel rodata    ───→  same PA              ┘
    Kernel text      ───→  same PA                 R-X
  0x80000000 (KERN_BASE)
    ...
  0x10000000
    UART0            ───→  0x10000000              RW-
  0x0C000000
    PLIC             ───→  0x0C000000              RW-
  ─────────────────────────────────────────────────────────────
```

The setup is just 4 `map_pages()` calls:

- **PLIC** (`0x0C000000`, 4 MB) — identity mapped, RW-
- **UART** (`0x10000000`, 4 KB) — identity mapped, RW-
- **Kernel text** (`KERN_BASE` to `_text_end`) — identity mapped, R-X
  (protects code from accidental writes)
- **`_text_end` to `PHYS_STOP`** — identity mapped, RW-. This single
  range covers rodata, data, BSS, stack, the struct page array, and
  all free memory. They're contiguous and share the same permissions,
  so there's no reason to map them separately.

CLINT (`0x02000000`) is **not** mapped — like xv6, we only access it
from M-mode (`timer_vec` in `entry.S`), which runs with `satp=0`
before paging is enabled. VIRTIO disk (`0x10001000`) is also not
mapped — bobchouOS does not yet have a disk driver.

No trampoline, no per-process kernel stacks, no guard pages — those
come in Phase 5/6 when we add processes.

> **Where should the kernel live in virtual memory?** Right now our
> identity map puts the kernel at virtual `0x8000_0000` — the lower
> half of the Sv39 space (bit 38 = 0). This works because we have no
> user processes to isolate from. When we add user processes, the
> convention is: lower half for user space, upper half (bit 38 = 1)
> for the kernel.
>
> xv6 takes a hybrid approach: identity map in the lower half, plus
> trampoline and kernel stacks mapped in the upper region near MAXVA.
> This works but is somewhat ad-hoc — the kernel's main address
> range sits in what is conventionally user territory. bobchouOS may
> move the kernel to the upper half in Phase 5/6, mapping virtual
> `0xFFFF_FFC0_8000_0000` → physical `0x8000_0000` (no longer
> identity-mapped, requiring PA↔VA conversion). This is a design
> decision we'll revisit when processes arrive.

> **Why separate text permissions?** In Phase 2, we already have the
> kernel text and data sections page-aligned in the linker script
> (`_text_end` is aligned to 4096). This lets us map `.text` as R-X
> and everything else as RW-. If a bug causes the kernel to write to
> its own code section, the hardware will raise a store page fault
> instead of silently corrupting instructions. xv6 does the same
> thing.

### One physical page, multiple virtual addresses

An important property of page tables that isn't obvious at first:
**the same physical page can be mapped at multiple virtual
addresses, each with different permissions.** Nothing in the hardware
prevents two PTEs (in the same page table, or in different ones)
from containing the same PPN. The hardware just follows each PTE
independently.

This isn't a bug — it's a feature that production kernels rely on
heavily. The clearest example is how Linux maps the kernel on RISC-V:

**Mapping 1: The linear map (all of RAM, coarse).** Linux maps *all*
physical memory into a contiguous virtual region using gigapages.
This gives the kernel a fast way to access any physical address.
But because it uses gigapages, the entire mapping gets uniform
**RW-** permissions — no execute, no per-section distinction:

```
  Virtual                     Physical          Perm   Size
  0xFFFF_FFC0_0000_0000  →   0x0000_0000_0000   RW-   1 GB gigapage
  0xFFFF_FFC0_4000_0000  →   0x0000_0040_0000   RW-   1 GB gigapage
  0xFFFF_FFC0_8000_0000  →   0x0000_0080_0000   RW-   1 GB gigapage
  ...
```

The kernel doesn't *execute* code through these addresses. It uses
them to read/write physical memory as data — zeroing pages, reading
page tables, copying data for syscalls.

**Mapping 2: The kernel image map (fine-grained).** Linux maps the
kernel binary itself at a different virtual base using 4 KB (or
2 MB) pages with proper per-section permissions:

```
  Virtual                     Physical          Perm   Section
  0xFFFF_FFFF_8020_0000  →   0x8020_0000        R-X   .text
  0xFFFF_FFFF_8020_1000  →   0x8020_1000        R-X   .text
  ...
  0xFFFF_FFFF_8020_4000  →   0x8020_4000        R--   .rodata
  0xFFFF_FFFF_8020_5000  →   0x8020_5000        RW-   .data
  0xFFFF_FFFF_8020_6000  →   0x8020_6000        RW-   .bss
```

The kernel's `pc` (program counter) uses *these* addresses. If a
bug tries to write to `.text` through this mapping, the hardware
raises a store page fault — protection works.

**The same physical page at two virtual addresses:**

```
  Physical page 0x8020_0000 (kernel .text):

  0xFFFF_FFC0_8020_0000  →  RW-   (linear map — for data access)
  0xFFFF_FFFF_8020_0000  →  R-X   (image map  — for execution)
```

This solves the superpages-vs-permissions dilemma: Linux gets
gigapage TLB efficiency for the linear map *and* fine-grained
protection for kernel code, by mapping the same physical memory
twice with different permissions.

> **Other uses of double mapping:**
> - **COW fork** — parent and child page tables both point to the
>   same physical page (read-only). When either writes, the page
>   fault handler copies the page. This is exactly why we built
>   `struct page` with a refcount in Phase 3.
> - **Shared memory** — two processes map the same physical pages
>   into their address spaces for fast inter-process communication.
> - **The trampoline** — xv6 maps a single physical page of
>   trampoline code at the top of *every* process's address space,
>   so the code is accessible at the same virtual address regardless
>   of which page table is active.

bobchouOS won't use double mapping in Phase 4 — our identity map
uses 4 KB pages with per-section permissions directly. But
understanding that physical pages can be multiply-mapped is essential
for later phases (COW fork, shared memory, trampoline).

### What about the PLIC?

We haven't talked about the PLIC (Platform-Level Interrupt
Controller) much yet — it handles external interrupts (from devices
like UART, virtio disk). Its registers live at physical address
`0x0C000000`. We'll need to map it because Phase 5 and beyond will
use it for device interrupts. We can identity-map it now and worry
about programming it later.

> **PLIC register space.** The PLIC spec allows registers spanning
> up to `0x0C000000` to `0x10000000` (64 MB), but xv6 only maps
> 4 MB (`0x400000`), which covers the priority, pending, enable, and
> threshold/claim registers. We'll do the same.

---

## Part 8: How xv6 Builds the Kernel Page Table

### xv6's `vm.c` — the key parts

xv6's virtual memory code is ~300 lines. Here's the structure,
stripped to the essentials:

```c
// kernel/vm.c (xv6-riscv), simplified

pagetable_t kernel_pagetable;

// Walk the 3-level page table, return pointer to the leaf PTE.
// If alloc=1 and intermediate table pages are missing, allocate them.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);    // follow pointer
        } else {
            if (!alloc) return 0;
            pagetable = (pagetable_t)kalloc();         // allocate new table page
            memset(pagetable, 0, PGSIZE);
            *pte = PA2PTE(pagetable) | PTE_V;          // install pointer
        }
    }
    return &pagetable[PX(0, va)];                      // return leaf PTE
}

// Install mappings for a VA→PA range, one page at a time.
void mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
    for (each page-aligned address a in [va, va+size)) {
        pte_t *pte = walk(pagetable, a, /*alloc=*/1);
        if (*pte & PTE_V) panic("remap");              // double-map guard
        *pte = PA2PTE(pa) | perm | PTE_V;
        pa += PGSIZE;
    }
}

// Thin wrapper — panics on failure.
void kvmmap(pagetable_t pt, uint64 va, uint64 pa, uint64 sz, int perm) {
    if (mappages(pt, va, sz, pa, perm) != 0) panic("kvmmap");
}

// Build the kernel page table.
void kvminit(void) {
    kernel_pagetable = (pagetable_t)kalloc();
    memset(kernel_pagetable, 0, PGSIZE);
    kvmmap(kernel_pagetable, UART0,    UART0,    PGSIZE,   PTE_R | PTE_W);
    kvmmap(kernel_pagetable, PLIC,     PLIC,     0x400000, PTE_R | PTE_W);
    kvmmap(kernel_pagetable, KERNBASE, KERNBASE, etext-KERNBASE, PTE_R | PTE_X);
    kvmmap(kernel_pagetable, etext,    etext,    PHYSTOP-etext,  PTE_R | PTE_W);
    kvmmap(kernel_pagetable, TRAMPOLINE, trampoline, PGSIZE, PTE_R | PTE_X);
    // ... plus VIRTIO, per-process kernel stacks
}

// Enable paging.
void kvminithart(void) {
    sfence_vma();                                    // ensure PTE writes are visible
    w_satp(MAKE_SATP(kernel_pagetable));             // paging ON
    sfence_vma();                                    // flush stale TLB entries
}
```

Key things to notice:

- **`walk()` is the core primitive.** It's the software equivalent of
  the hardware page table walker — same 3-level descent, same
  VPN indexing. But it's used to *build and modify* page tables, not
  to translate at runtime. When `alloc=1`, it creates missing
  intermediate table pages on the fly with `kalloc()`.

- **`mappages()` panics on remap.** If a PTE already has V=1, it's
  a bug — you're overwriting an existing mapping. This is why you
  can't "map everything RW- then change text to R-X" — the second
  mapping attempt would hit this guard.

- **`kvminit()` is just a sequence of `kvmmap()` calls.** Allocate
  an empty root page, then add identity-mapped regions one by one
  with different permissions. The pattern is always:
  `kvmmap(table, va, pa, size, permissions)` where `va == pa` for
  identity mappings.

- **Two `sfence.vma` in `kvminithart()`.** The first ensures prior
  page table writes are visible to the hardware walker before the new
  `satp` takes effect. The second flushes stale TLB entries from Bare
  mode. Between these two fences, `satp` is written and paging turns
  on.

- **Helper macros.** `PX(level, va)` extracts the 9-bit VPN index:
  `(va >> (12 + 9*level)) & 0x1FF`. `PA2PTE(pa)` converts a physical
  address to PTE bits: `(pa >> 12) << 10` (shift right to remove the
  page offset, left to position into bits 53:10 — the 2-bit gap is
  the RSW field). `PTE2PA(pte)` reverses it: `(pte >> 10) << 12`.

### The boot sequence

```
main()
  → kinit()           // initialize page allocator
  → kvminit()         // build kernel page table (satp still 0)
  → kvminithart()     // write satp → paging is ON
  → ... rest of init  // everything now runs with virtual memory
```

Between `kvminit()` and `kvminithart()`, the kernel is still running
with `satp=0` (Bare mode). The page table is built in physical
memory using physical addresses. When `kvminithart()` writes `satp`,
the identity mapping ensures the transition is seamless — the next
instruction fetch still works because virtual == physical for the
kernel region.

---

## Part 9: bobchouOS Design Decisions

### bobchouOS vs. xv6 comparison

| Aspect | xv6 | bobchouOS (Phase 4) |
|---|---|---|
| Paging mode | Sv39 | Sv39 |
| Kernel mapping | Identity + trampoline + kstacks | Identity only (trampoline/kstacks in Phase 5/6) |
| Text permissions | R-X | R-X |
| Data permissions | RW- | RW- |
| MMIO mapping | UART, VIRTIO, PLIC | UART, PLIC |
| CLINT mapped? | No (M-mode only) | No (same reason) |
| A/D bits | Left to hardware | Pre-set to 1 (avoid faults) |
| `walk()` alloc strategy | Allocate on demand via `alloc` flag | Same approach |
| TLB flush | `sfence.vma zero, zero` always | Same |
| ASID | Not used (always 0) | Not used |
| PTE type | `pte_t` = `uint64` | Same — clearly expresses 8-byte entry |
| Page table pointer | `pagetable_t` = `uint64*` | Just use `pte_t*` directly |
| Super pages | Not used | Not used |

### Naming conventions for Phase 4

Following bobchouOS naming conventions:

| xv6 name | bobchouOS name | Notes |
|---|---|---|
| `kvminit` | `vm_init` | Module-prefixed, like `kalloc_init` |
| `kvminithart` | `vm_init_hart` | Clearer word separation |
| `kvmmap` | `kvm_map` | Maps into kernel page table |
| `mappages` | `map_pages` | snake_case |
| `walk` | `walk` | Already a single word |
| `kvmmake` | (not needed) | xv6 separates make/init for multi-hart |
| `PGSIZE` | `PG_SIZE` | Already defined in Phase 3 |
| `PGROUNDUP` | `PG_ROUND_UP` | Already defined in Phase 3 |
| `PTE_V`, `PTE_R`, ... | `PTE_V`, `PTE_R`, ... | Same — these are standard |
| `pagetable_t` | (drop) | Use `pte_t*` directly |
| `PA2PTE` | `pa_to_pte` | Consistent with `pa_to_page` convention |
| `PTE2PA` | `pte_to_pa` | Same |
| `MAXVA` | `MAX_VA` | snake_case |

### Our implementation plan

### Files

```
kernel/
    include/
        vm.h              <-- NEW: pte_t, PTE flags, inline helpers, declarations
        riscv.h           <-- UPDATE: add sfence_vma(), SATP_SV39, MAKE_SATP
        mem_layout.h      <-- UPDATE: add PLIC_BASE, PLIC_SIZE
    vm.c                  <-- NEW: walk, map_pages, vm_init, vm_init_hart
    main.c                <-- UPDATE: call vm_init() and vm_init_hart()
    test/
        test_vm.c         <-- NEW: page table tests
        run_tests.c       <-- UPDATE: add test_vm()
Makefile                  <-- UPDATE: add vm.o and test_vm.o
```

### `vm.h`

Defines `pte_t` as `uint64`, PTE flag constants (`PTE_V` through
`PTE_D`), inline conversion helpers (`pa_to_pte`, `pte_to_pa`,
`pte_flags`), the `PX(level, va)` macro for extracting VPN indices,
`MAX_VA`, and function declarations for `vm_init`, `vm_init_hart`,
`walk`, and `map_pages`.

Key design choices:
- `pa_to_pte` / `pte_to_pa` are `static inline` functions (not
  macros) — type-safe, no double-evaluation, same pattern as
  `csrr`/`csrw`
- `PX` stays a macro — it's a pure utility like `PG_ROUND_UP`
- Page table pointers are `pte_t*` directly (no `pagetable_t`
  typedef)

### Updates to `riscv.h`

Add to the C-only section:
- `sfence_vma()` — `static inline` wrapping
  `asm volatile("sfence.vma zero, zero")`
- `SATP_SV39` — `(8UL << 60)`
- `MAKE_SATP(pagetable)` — combines `SATP_SV39` with the root
  page table's PPN

### Updates to `mem_layout.h`

Add `PLIC_BASE` (`0x0C000000`) and `PLIC_SIZE` (`0x400000` = 4 MB,
matching xv6).

### `vm.c` — key functions

**`walk(pagetable, va, alloc)`** — descends the 3-level tree from
level 2 to level 0, using `PX(level, va)` to index each table page.
At each non-leaf level: if the PTE is valid, follow the pointer via
`pte_to_pa()`. If invalid and `alloc=1`, allocate a new table page
with `kalloc()`, zero it, and install a pointer PTE. Returns a
pointer to the level-0 (leaf) PTE.

**`map_pages(pagetable, va, size, pa, perm)`** — iterates page by
page over the VA range. For each page: calls `walk(alloc=1)` to find
or create the leaf PTE, panics if already mapped (V=1), then writes
`pa_to_pte(pa) | perm | PTE_V | PTE_A | PTE_D`. The A/D bits are
pre-set to avoid page faults on hardware that doesn't set them
automatically.

**`kvm_map(va, pa, size, perm)`** — thin wrapper that calls
`map_pages()` on `kernel_pagetable` and panics on failure.

**`vm_init()`** — allocates and zeroes the root page table page,
then calls `kvm_map()` for each region: PLIC (RW), UART (RW),
kernel text (R-X), and `_text_end` through `PHYS_STOP` (RW).

**`vm_init_hart()`** — `sfence_vma()`, write `satp` with
`MAKE_SATP(kernel_pagetable)`, `sfence_vma()` again. Paging is on.

### Differences from xv6

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Page table pointer type | `pagetable_t` = `uint64*` | `pte_t*` directly |
| PA↔PTE conversion | `PA2PTE` / `PTE2PA` macros | `pa_to_pte` / `pte_to_pa` inline functions |
| A/D bits | Left to hardware | Pre-set to 1 in `map_pages()` |
| MMIO mapped | UART, VIRTIO, PLIC | UART, PLIC (no VIRTIO — no disk driver yet) |
| CLINT mapped | No | No |
| Trampoline | Mapped at MAXVA | Not yet (Phase 5/6) |
| Kernel stacks | Per-process with guard pages | Not yet (Phase 5/6) |
| Init function | `kvminit()` + `kvminithart()` | `vm_init()` + `vm_init_hart()` |
| Map helper | `kvmmap()` | `kvm_map()` |
| Test coverage | None | `test_vm.c` |

> **Note on PTE_A and PTE_D.** We pre-set both accessed and dirty
> bits in every PTE via `map_pages()`. This avoids page faults on
> first access from hardware that raises a fault when A=0 or D=0
> instead of setting them automatically. QEMU sets them automatically,
> but pre-setting is safer and portable.

### Updating `kmain()`

We add two lines after `kalloc_init()`:

```c
void
kmain(void) {
    uart_init();
    csrw(stvec, (uint64)kernel_vec);
    csrw(sie, csrr(sie) | SIE_SSIE);
    csrw(sstatus, csrr(sstatus) | SSTATUS_SIE);

    kprintf("\nbobchouOS is booting...\n");
    ...

    kalloc_init();

    vm_init();        // <-- NEW: build kernel page table
    vm_init_hart();   // <-- NEW: enable paging (Sv39)

    ...
}
```

`vm_init()` must come after `kalloc_init()` (it calls `kalloc()` to
allocate page table pages). `vm_init_hart()` must come after
`vm_init()` (it installs the page table that `vm_init()` built).

### Expected boot output

```
bobchouOS is booting...
running in S-mode
sstatus=0x8000000200006022
kernel: 0x80000000 .. 0x8000XXXX (NNNNN bytes)
kalloc_init: 32739 free pages (130956 KB), page array = 64 KB
vm_init: kernel page table at 0x8XXXXXXX
vm_init_hart: paging enabled (Sv39)

timer interrupts enabled, waiting for ticks...
timer: 1 seconds
timer: 2 seconds
...
```

After `vm_init_hart`, paging is on but the kernel continues normally
— the identity mapping means all existing pointers and addresses
remain valid. Timer interrupts keep firing. The only difference is
that unmapped addresses now cause page faults instead of undefined
behavior.

---

## Part 10: Understanding Page Faults (Preview)

### What happens when translation fails

We defined the exception codes for page faults back in Phase 2:

| scause | Exception | When |
|---|---|---|
| 12 | Instruction page fault | Instruction fetch from unmapped/non-executable page |
| 13 | Load page fault | `ld`/`lb`/etc. from unmapped/non-readable page |
| 15 | Store page fault | `sd`/`sb`/etc. to unmapped/non-writable page |

These are the same exception codes in our `riscv.h`:

```c
#define EXC_INST_PAGE    12
#define EXC_LOAD_PAGE    13
#define EXC_STORE_PAGE   15
```

When a page fault occurs, the hardware:

1. Sets `scause` to 12, 13, or 15
2. Sets `sepc` to the address of the faulting instruction
3. Sets `stval` to the **virtual address** that caused the fault
4. Traps into the kernel via `stvec`

Our `kernel_trap()` handler already has a default case that panics on
unhandled exceptions — so if we accidentally access an unmapped page
after enabling paging, we'll get a diagnostic panic showing the
faulting address. In Phase 6, page faults become useful: they enable
lazy allocation (allocate pages on first access) and COW fork (copy
pages only when written).

> **Page faults are not errors.** In modern kernels, page faults are
> a feature, not a bug. They let the kernel defer work until it's
> actually needed. A page fault says "the hardware couldn't translate
> this address — kernel, please fix the page table and retry." The
> kernel can respond by allocating a page, copying data, loading from
> swap, or killing the process — whatever is appropriate. Only a
> page fault with *no* valid response is an error.

---

## Part 11: Putting It All Together

### The full picture

Here's the boot sequence after Phase 4 is implemented:

```
entry.S (_start):
    Zero BSS, set up stack
    Configure timer, PMP
    mret → kmain (S-mode, satp=0, paging OFF)

kmain():
    uart_init()
    Set stvec, enable interrupts
    Print boot banner

    kalloc_init()
        Build struct page array
        Build free list (31K pages)

    vm_init()                          ← NEW
        Allocate root page table page (kalloc)
        Map PLIC     (identity, RW)
        Map UART     (identity, RW)
        Map kernel text (identity, R-X)
        Map _text_end through PHYS_STOP (identity, RW)

    vm_init_hart()                     ← NEW
        sfence.vma
        csrw satp, MAKE_SATP(kernel_pagetable)
        sfence.vma
        ─── paging is ON ───
        (kernel continues running — identity mapping means
         same virtual addresses still work)

    [tests, timer loop, ...]
```

### What changes when paging is on?

From the kernel's perspective: **almost nothing**. Because the
kernel page table is an identity map, every pointer the kernel holds
is still valid. `kalloc()` returns a physical address, and because
virtual == physical, the kernel can dereference it directly. Device
registers at `0x10000000` (UART) are identity-mapped, so `uart.c`
works unchanged.

The only visible change is protection:

- Writing to the `.text` section will now page-fault instead of
  silently succeeding
- Accessing an unmapped address will page-fault instead of... well,
  on QEMU without paging, unmapped addresses either hit DRAM (if in
  range) or cause a bus error. With paging, you get a clean page
  fault with `stval` telling you exactly which address was bad.

### What's next

The implementation plan above describes the skeleton. After
implementing the TODOs, paging will be enabled and all 
tests should pass.

**A later round** adds:

- `kmalloc()` / `kmfree()` — kernel heap allocator for sub-page
  allocations (process structs, file descriptors, etc.)

---

## Quick Reference

### Sv39 virtual address decomposition

```
  63      39 38      30 29      21 20      12 11            0
  +---------+----------+----------+----------+--------------+
  |   EXT   |  VPN[2]  |  VPN[1]  |  VPN[0]  |  page offset |
  +---------+----------+----------+----------+--------------+
    25 bits     9 bits     9 bits     9 bits      12 bits
```

### Sv39 PTE format

```
  Bit(s)   Field    Meaning
  ------   -----    -------
  0        V        Valid
  1        R        Readable
  2        W        Writable
  3        X        Executable
  4        U        User-mode accessible
  5        G        Global
  6        A        Accessed
  7        D        Dirty
  9:8      RSW      Reserved for software
  53:10    PPN      Physical Page Number (44 bits)
  63:54    Reserved Must be zero
```

### `satp` CSR format (RV64)

```
  63:60    MODE     0=Bare, 8=Sv39, 9=Sv48, 10=Sv57
  59:44    ASID     Address Space IDentifier
  43:0     PPN      Root page table physical page number
```

### Key constants and helpers

| Name | Definition | Purpose |
|---|---|---|
| `PTE_V` | `(1 << 0)` | Valid bit |
| `PTE_R` | `(1 << 1)` | Read permission |
| `PTE_W` | `(1 << 2)` | Write permission |
| `PTE_X` | `(1 << 3)` | Execute permission |
| `PTE_U` | `(1 << 4)` | User-accessible |
| `PTE_G` | `(1 << 5)` | Global mapping |
| `PTE_A` | `(1 << 6)` | Accessed |
| `PTE_D` | `(1 << 7)` | Dirty |
| `pa_to_pte(pa)` | `((pa >> 12) << 10)` | Physical addr → PTE bits |
| `pte_to_pa(pte)` | `((pte >> 10) << 12)` | PTE bits → physical addr |
| `PX(level, va)` | `(va >> (12+9*level)) & 0x1FF` | Extract VPN index |
| `MAKE_SATP(pt)` | `(8UL<<60) \| ((pt)>>12)` | Build satp value |
| `MAX_VA` | `(1UL << 39)` | End of Sv39 address space |

### Page table walk summary

```
  Input: satp, virtual address VA
  1. root = satp.PPN << 12
  2. L2 PTE = *(root + VPN[2] * 8)     — check V, leaf?
  3. L1 PTE = *(L2.PPN<<12 + VPN[1]*8)  — check V, leaf?
  4. L0 PTE = *(L1.PPN<<12 + VPN[0]*8)  — check V, must be leaf
  5. PA = (L0.PPN << 12) | offset
```

### bobchouOS kernel page table mappings (Phase 4)

| Virtual range | Physical range | Size | Permissions | Purpose |
|---|---|---|---|---|
| `0x0C000000` | `0x0C000000` | 4 MB | RW- | PLIC |
| `0x10000000` | `0x10000000` | 4 KB | RW- | UART0 |
| `KERN_BASE`–`_text_end` | same | ~16 KB | R-X | Kernel text |
| `_text_end`–`PHYS_STOP` | same | ~128 MB | RW- | rodata + data + BSS + struct page + free mem |
