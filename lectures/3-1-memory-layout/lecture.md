# Lecture 3-1: Physical Memory Layout

> **Where we are**
>
> Phase 2 is complete. The kernel boots into S-mode, handles timer
> interrupts (10 ms tick, printing once per second), and dispatches
> exceptions with human-readable names. We have a working trap
> infrastructure, a test framework with 14 passing tests, and a kernel
> that prints boot diagnostics showing the kernel's address range.
>
> But the kernel can't *allocate* memory. Every data structure so far
> lives in `.data` or `.bss` (sized at compile time) or on the stack
> (scoped to a function call). If we need a page table, a process
> control block, or a buffer at runtime, there's nowhere to get one.
>
> Phase 3 fixes that. We're building a physical page allocator — the
> lowest layer of the kernel's memory management. Before we write the
> allocator code (Round 3-2), we need to answer a more basic question:
> **what memory is available?** Which addresses are RAM? Which are
> device registers? Where does the kernel end and the free region
> begin? How big is a "page"?
>
> This lecture maps out the physical address space of QEMU's `virt`
> machine, defines the constants and macros the allocator needs, and
> establishes the vocabulary (page, page-aligned, PGSIZE, PGROUNDUP)
> that every subsequent phase builds on.
>
> By the end of this lecture, you will understand:
>
> - The complete physical address map of the QEMU `virt` machine
> - Why DRAM starts at 0x80000000, not 0x0
> - Where MMIO devices live and why they share the physical address space
> - How the linker script carves up the DRAM region for the kernel
> - What a "page" is and why 4096 bytes
> - The free memory region: from kernel end to PHYSTOP
> - Page alignment macros (PGROUNDUP, PGROUNDDOWN) and why they matter
> - How xv6's `memlayout.h` organizes these constants
> - What we need now vs. what we'll add in Phase 4 (virtual memory)
>
> **xv6 book coverage:** This lecture absorbs Chapter 3, sections 3.1
> (Paging hardware — the address space overview) and 3.4 (Physical
> memory allocation — specifically the part about KERNBASE, PHYSTOP,
> and the free region). The page table mechanism itself (Sv39, PTEs,
> three-level walks) is deferred to Phase 4. Section 3.5's allocator
> code is deferred to Round 3-2.

---

## Part 1: The Physical Address Space

### What is a physical address?

Every byte of memory and every device register on the system has a
**physical address** — a number that the CPU puts on the address bus
when it wants to read or write that location. On RISC-V, physical
addresses are 64 bits wide (though most implementations only use
56 bits, and QEMU's `virt` machine uses fewer still).

Right now, with paging disabled (`satp = 0`), the address in every
`ld`/`sd` instruction *is* the physical address. The CPU sends it
directly to the memory bus. In Phase 4, when we enable paging, the
address in the instruction becomes a *virtual* address, and the
hardware translates it to a physical address via the page table.
But for now, physical = virtual — what you write is what the bus sees.

### Not all addresses are RAM

A common misconception: "physical address" does not mean "RAM." The
physical address space is shared between RAM and memory-mapped I/O
(MMIO) devices. When the CPU issues a load or store to address
`0x10000000`, the memory controller doesn't route it to a RAM chip —
it routes it to the UART device. The UART responds with a byte of
data (or accepts a byte for transmission).

This is how all hardware communication works on RISC-V (and ARM, and
most modern architectures). There are no special I/O instructions like
x86's `in`/`out`. Reading a device register is literally a `ld`
instruction. Writing one is literally a `sd` instruction. The address
determines whether the access hits RAM, a UART, a timer, or an
interrupt controller.

> **Comparison: how other architectures handle I/O**
>
> **x86** has *two* address spaces:
>
> 1. **Memory address space** — accessed by `mov`, etc. Contains RAM
>    and some MMIO devices.
> 2. **I/O port space** — a separate 64 KB space accessed by special
>    `in`/`out` instructions. Legacy devices (PS/2 keyboard, serial
>    ports, PIC) live here.
>
> Modern x86 hardware (PCIe devices, APIC, HPET) uses memory-mapped
> I/O; the port space is a legacy holdover from the 8086 era. But it
> still exists, and Linux still uses `in`/`out` for legacy devices.
> Even *interrupts* moved to MMIO: Message Signaled Interrupts (MSI/
> MSI-X, introduced in PCI 2.2) let a device signal an interrupt by
> writing to a special memory address — no dedicated interrupt pins, no
> `in`/`out`, just a regular memory write with guaranteed ordering
> relative to prior data writes.
>
> **ARM64 (AArch64)** is purely memory-mapped, just like RISC-V. All
> device access goes through regular load/store instructions (`ldr`/
> `str`). There are no port I/O instructions. ARM went memory-mapped
> from the start — it never had a separate I/O space to begin with.
> ARM does add device memory type attributes (Device-nGnRnE, etc.) in
> its page tables to control ordering and caching of MMIO accesses, but
> the instructions themselves are the same `ldr`/`str` you use for RAM.
>
> **RISC-V** follows ARM's approach: one address space, everything
> memory-mapped. One set of load/store instructions handles both RAM
> and devices. This is the simplest design — no special I/O
> instructions to learn, no separate address space to manage.
>
> | Architecture | I/O mechanism | Special I/O instructions? |
> |-------------|--------------|:-------------------------:|
> | x86 | Port I/O + MMIO | Yes (`in`/`out`) |
> | ARM64 | MMIO only | No |
> | RISC-V | MMIO only | No |
>
> **Where do these architectures live?** You'll encounter all three in
> practice, but in very different places:
>
> | Segment | Dominant | Rising |
> |---------|----------|--------|
> | Server / data center | x86-64 (Xeon, EPYC) | ARM64 (Graviton, Ampere) |
> | Desktop / laptop | x86-64 (Core, Ryzen) | ARM64 (Apple M-series, Snapdragon X) |
> | Mobile (phone/tablet) | ARM64 (~100%) | — |
> | Embedded / IoT | ARM Cortex-M | RISC-V (ESP32-C3, GD32V) |
> | Game consoles | x86-64 (PS5, Xbox) | ARM (Nintendo Switch) |
>
> The pattern: **x86-64 dominates anything with a keyboard, ARM
> dominates anything with a battery, and RISC-V is the insurgent**
> eating into embedded first with aspirations upward.
>
> The market map is shaped by **licensing models**:
>
> - **x86-64 is closed.** Only Intel and AMD can make x86 chips — they
>   cross-license each other but don't sell licenses to outsiders. Even
>   with unlimited money and talent, you legally cannot build an x86-64
>   processor. This is why Apple couldn't design their own x86 laptop
>   chip and had to either keep buying from Intel or switch ISAs.
> - **ARM is licensable.** ARM Holdings sells ISA licenses (and
>   optionally pre-designed core blueprints) to chip vendors. Pay a
>   license fee + per-chip royalty, and you can design custom ARM cores.
>   Apple chose this route — they'd been designing custom ARM cores for
>   iPhones since the A4 (2010), then scaled the same approach up to
>   laptops with the M1 (2020).
> - **RISC-V is open.** The ISA spec is free — anyone can design a
>   RISC-V core without paying or asking permission. "Free ISA" doesn't
>   mean "free chip" (design, verification, and fabrication still cost
>   millions), but the *permission to start* costs nothing. This is why
>   companies like Alibaba (Xuantie cores), SiFive, and ESP32 vendors
>   are adopting it — the same freedom Apple has with ARM, minus the
>   licensing fees. The tradeoff is a less mature ecosystem (fewer
>   tools, libraries, OS support), which is shrinking every year.
>
> We're building on RISC-V partly because it's the cleanest ISA to
> learn on, and partly because it's the architecture whose market share
> is growing fastest from zero.

### The QEMU `virt` machine address map

QEMU's `virt` machine defines a fixed physical address layout. The
addresses are hardcoded in QEMU's source code (`hw/riscv/virt.c`).
Here's the full map of the regions we care about:

```
Physical Address          Size        Device / Region
-------------------       ---------   ----------------------------------------
0x0000_0000               varies      (debug, test devices — not used)
0x0000_0100               ---         (reserved)
0x0000_1000               0x100       Boot ROM (holds reset vector)
0x0010_0000               0x1000      SiFive Test (poweroff/reboot via MMIO write)
0x0010_1000               0x1000      Goldfish RTC (real-time clock)
0x0200_0000               0x10000     CLINT (Core Local Interruptor)
                                        +0x0000: msip (per-hart)
                                        +0x4000: mtimecmp (per-hart)
                                        +0xBFF8: mtime (shared)
0x0C00_0000               0x600000    PLIC (Platform-Level Interrupt Controller)
0x1000_0000               0x100       UART0 (16550-compatible serial port)
0x1000_1000               0x1000      virtio-mmio device 0
  ...                                 (virtio devices 1-7, up to 0x1000_8000)
0x1010_0000               0x18        fw-cfg (QEMU firmware config interface)
0x2000_0000               0x4000000   CFI Flash (2 banks, 32 MB each)
0x3000_0000               0x10000000  PCIe ECAM (PCI config space)
0x4000_0000               0x40000000  PCIe MMIO (PCI device memory window)
0x8000_0000               0x8000000   DRAM (128 MB, default)
  ^                                     ^
  KERNBASE                              PHYSTOP = KERNBASE + 128MB
```

Most of these devices we'll never touch in bobchouOS, but here's what
they are so the map isn't just mysterious addresses:

- **SiFive Test** — a trivial "syscon" device. Write `0x5555` to it
  and QEMU powers off; write `0x7777` and QEMU reboots. That's the
  entire interface. Useful for `shutdown()` / `reboot()` if we ever
  implement them.

  > **How does reboot actually work on real hardware?** The pattern is
  > always the same: write something to some hardware that controls the
  > power/reset line. But *which* hardware varies wildly:
  >
  > - **QEMU `virt` (RISC-V):** One MMIO store to the SiFive Test
  >   register. Linux discovers it from the device tree and registers
  >   it as the `syscon-reboot` handler.
  > - **Real RISC-V hardware:** An SBI call — `sbi_shutdown()`. The
  >   firmware (OpenSBI) talks to whatever platform-specific power
  >   controller exists.
  > - **x86:** The fun one. Linux tries multiple methods in sequence
  >   because decades of hardware diversity means no single method
  >   works everywhere:
  >   1. **ACPI reset** — write a value to the ACPI-specified reset
  >      register (address and value come from the ACPI FADT table).
  >      This is the modern, clean path.
  >   2. **Keyboard controller** — send `0xFE` to I/O port `0x64`.
  >      Yes, the PS/2 keyboard controller has a "reset CPU" command.
  >      This dates back to the IBM AT (1984) where the keyboard
  >      controller had a spare pin wired to the CPU reset line. It's
  >      absurd, but it still works on most hardware today.
  >   3. **CF9 port** — write to I/O port `0xCF9` (Intel chipset full
  >      reset register).
  >   4. **Triple fault** — load an invalid IDT (interrupt descriptor
  >      table), then trigger an interrupt. The CPU can't find a
  >      handler, can't find the double-fault handler either, and
  >      gives up — a "triple fault" forces a hardware CPU reset.
  >      This is the nuclear option.
  >
  > For **shutdown** on x86, the path goes through ACPI: write to
  > the `PM1a_CNT` register with the `SLP_TYP` value for S5 (soft-off
  > state). The chipset cuts power.
  >
  > The keyboard controller trick is a classic piece of computing
  > archaeology — a decision made in 1984 (wire a spare pin on the
  > keyboard chip to CPU reset) is still a supported reboot path in
  > Linux 40 years later.

- **Goldfish RTC** — a wall-clock timer (year/month/day/hour/minute).
  The CLINT gives us a monotonic tick counter, but not the actual date.
  The RTC fills that gap. Named after Google's "Goldfish" virtual
  platform (originally for Android emulation).
- **fw-cfg** — a QEMU-specific interface for passing configuration
  data (kernel command line, initrd location, etc.) from the host to
  the guest. Only exists in QEMU — real hardware wouldn't have this.
- **CFI Flash** — NOR flash memory, the kind used for BIOS/firmware
  storage. QEMU emulates two 32 MB banks. A real OS could store a
  filesystem here; we won't use it.
- **PCIe ECAM** — the PCI Express configuration space. Every PCI
  device has a block of configuration registers (vendor ID, device ID,
  BARs, interrupt routing). ECAM (Enhanced Configuration Access
  Mechanism) maps all of them into a contiguous MMIO region so the OS
  can discover and configure PCI devices with plain memory reads/writes.
- **PCIe MMIO** — the memory window where PCI devices expose their
  own registers and buffers. When a PCI device (network card, GPU,
  NVMe controller) needs memory-mapped I/O, the OS assigns addresses
  from this 1 GB window via the device's BAR (Base Address Register).

Three things to notice:

**1. The I/O hole below 0x80000000.** The bottom 2 GB of the physical
address space is reserved for MMIO devices. Nothing between 0x0 and
0x80000000 is RAM. This is why reading address `0x0` doesn't give you
"the first byte of memory" — there's no RAM there. On some platforms,
accessing an unmapped address in this range causes a bus error (access
fault exception, cause 5 or 7).

**2. DRAM starts at 0x80000000.** This is a RISC-V convention, not a
hardware requirement. The `virt` machine, SiFive boards, and most
RISC-V platforms place DRAM at `0x80000000`. QEMU loads the kernel
image (our `kernel.elf`) at this address, and the CPU begins executing
from here (or from a boot ROM that jumps here). We defined
`_start = 0x80000000` in our linker script.

**3. DRAM is 128 MB by default.** QEMU's `virt` machine gives us
128 MB (0x8000000 bytes) of RAM, from `0x80000000` to `0x88000000`.
This is configurable with `-m`, but 128 MB is the default we'll use.
xv6 hardcodes this same assumption.

### We already know some of these addresses

Look at our existing code — we've been using physical addresses all
along:

| Address | Where we use it | File |
|---------|----------------|------|
| `0x02000000` (CLINT_BASE) | Timer programming | `riscv.h`, `entry.S` |
| `0x10000000` (UART0) | Serial I/O | `uart.c` |
| `0x80000000` (`. = 0x80000000`) | Kernel load address | `linker.ld` |

What we haven't named yet: the *end* of RAM (`0x88000000`) and the
concept of the free region between the kernel and that end. That's
what `mem_layout.h` will define.

---

## Part 2: Why DRAM Starts at 0x80000000

### The short answer

Convention. The RISC-V platform spec doesn't mandate a DRAM base
address, but `0x80000000` (2 GB) has become the de facto standard
across QEMU `virt`, SiFive HiFive boards, Kendryte K210, and others.

### The longer answer

The physical address space needs room for both MMIO devices and RAM.
Devices need stable, well-known addresses (so firmware and drivers can
find them without probing). RAM can start anywhere — the bootloader or
device tree tells the kernel where it is. So platforms put devices in
the low addresses (where they're easy to address with short offsets)
and RAM above them.

The specific value `0x80000000` gives 2 GB of address space for MMIO
devices below it. That's generous — QEMU's `virt` machine uses only a
tiny fraction (CLINT at 0x02000000, PLIC at 0x0C000000, UART at
0x10000000). But it leaves room for expansion without ever needing to
relocate RAM.

> **How does the kernel discover the memory layout?**
>
> On real hardware, the bootloader passes a **device tree blob (DTB)**
> — a data structure that describes the machine: how much RAM, where
> devices are, how many harts, etc. The kernel parses the DTB at boot
> to discover the physical address map.
>
> xv6 skips this entirely and hardcodes the addresses. We do the same.
> For QEMU `virt`, the layout is fixed and documented, so hardcoding
> is fine. A production OS would parse the device tree.
>
> QEMU actually does pass a DTB to the kernel (in register `a1` at
> boot). We ignore it. If you're curious, you can dump it:
>
> ```bash
> qemu-system-riscv64 -machine virt -machine dumpdtb=virt.dtb -nographic
> dtc -I dtb -O dts virt.dtb
> ```
>
> The output shows every device, its address, its interrupts, and the
> memory ranges. It's the "ground truth" for the machine's layout.

---

## Part 3: How the Kernel Occupies DRAM

### The linker script layout

Our linker script (`linker.ld`) places the kernel at the base of
DRAM:

```
0x80000000  +------------------+ <-- _kernel_start
            |  .text           |     Code (entry.S first)
            +------------------+ <-- _text_end (page-aligned)
            |  .rodata         |     Read-only data (string literals, tables)
            +------------------+
            |  .data           |     Initialized globals
            +------------------+ <-- _bss_start
            |  .bss            |     Uninitialized globals (zeroed by entry.S)
            +------------------+ <-- _bss_end
            |  16 KB stack     |     Boot/kernel stack (grows downward)
            +------------------+ <-- stack_top = _kernel_end
```

All of these symbols (`_kernel_start`, `_text_end`, `_bss_start`,
`_bss_end`, `_kernel_end`) are defined in `linker.ld` and declared as
`extern char[]` in C code. We already use `_kernel_start` and
`_kernel_end` in `kmain()` to print the kernel's size.

### What uses each section?

| Section | Contents | Examples from our code |
|---------|----------|----------------------|
| `.text` | Machine instructions | `_start`, `kernel_vec`, `kmain()`, `kprintf()` |
| `.rodata` | Read-only data | `exc_names[]` string table, format strings |
| `.data` | Initialized globals | (currently none — we initialize at runtime) |
| `.bss` | Zero-initialized globals | `ticks` counter, `timer_scratch` array |
| stack | Function call frames | Local variables, return addresses |

### How big is the kernel right now?

The `kmain()` boot message tells us:

```
kernel: 0x80000000 .. 0x8000XXXX (NNNNN bytes)
```

It's small — a few tens of kilobytes. The important point is that
everything from `0x80000000` to `_kernel_end` is **occupied by the
kernel** and must not be touched by the allocator. Everything above
`_kernel_end` (up to the end of RAM) is **free** — available for
the allocator to hand out.

---

## Part 4: The Free Region

### The picture

```
0x80000000  +------------------+ <-- KERNBASE / _kernel_start
            |                  |
            |  Kernel image    |     .text, .rodata, .data, .bss, stack
            |  (occupied)      |
            |                  |
            +------------------+ <-- _kernel_end
            |                  |
            |  FREE MEMORY     |     Available for kalloc()
            |                  |     The page allocator owns this region
            |  (many MB)       |
            |                  |
            +------------------+ <-- PHYSTOP (0x88000000)
```

The free region is:

- **Start:** `_kernel_end` (rounded up to the next page boundary)
- **End:** `PHYSTOP` (0x88000000 = KERNBASE + 128 MB)
- **Size:** roughly 128 MB minus the kernel image size

This is where every `kalloc()` will come from — every page table,
every process stack, every pipe buffer. When `kalloc()` returns NULL,
we're out of physical memory.

> **What's above PHYSTOP?** Nothing. QEMU's `virt` machine only maps
> 128 MB of DRAM. Addresses above `0x88000000` don't correspond to any
> RAM chip or device — the memory controller has nothing to route the
> request to. A load or store there would cause an access fault
> (exception cause 5 or 7).
>
> You *can* give QEMU more RAM with `-m`:
>
> ```
> qemu-system-riscv64 -machine virt -m 256M ...  # DRAM: 0x80000000 - 0x90000000
> qemu-system-riscv64 -machine virt -m 1G ...    # DRAM: 0x80000000 - 0xC0000000
> ```
>
> In that case PHYSTOP would need to move accordingly. We hardcode
> 128 MB because xv6 does, and it's plenty for our needs.
>
> On real hardware, the physical address space above DRAM might contain
> **high MMIO regions** — some platforms place PCIe device BARs or
> additional flash above the DRAM window. But on QEMU `virt` with
> default settings, it's simply empty. The PCIe windows we saw in the
> address map (0x30000000, 0x40000000) live *below* DRAM, not above it.

### Why round up to a page boundary?

The allocator works in whole pages (4096 bytes). If `_kernel_end` is
at `0x8000_A234`, we can't hand out the region `0x8000_A234` to
`0x8000_AFFF` as a page — it's not a full 4096 bytes and it's not
page-aligned. We round up to `0x8000_B000` and start allocating from
there. The few bytes between `_kernel_end` and the next page boundary
are wasted (internal fragmentation), but it's a one-time loss of at
most 4095 bytes — trivial.

### How xv6 defines the free region

xv6 uses the same approach. In `kernel/kalloc.c`:

```c
extern char end[];     // first address after kernel, from linker script

void
kinit()
{
    freerange(end, (void*)PHYSTOP);
}
```

`end` is xv6's equivalent of our `_kernel_end`. The symbol `PHYSTOP`
is defined in `kernel/memlayout.h`:

```c
// kernel/memlayout.h (xv6)
#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128*1024*1024)
```

`freerange()` takes the start and end of the free region and calls
`kfree()` on every page in that range, building the free list. We'll
walk through this in detail in the Round 3-2 lecture.

---

## Part 5: Pages and Page Alignment

### What is a page?

A **page** is the smallest unit of memory that the kernel allocates
and manages. On RISC-V (and x86, and ARM), the standard page size is
**4096 bytes** (4 KB, or 0x1000 in hex).

Why 4096? It's a tradeoff:

| Factor | Smaller pages (e.g., 1 KB) | Larger pages (e.g., 64 KB) |
|--------|---------------------------|---------------------------|
| Internal fragmentation | Less waste (allocations fit tighter) | More waste (small allocations waste most of the page) |
| Page table size | Larger (more entries needed) | Smaller (fewer entries) |
| TLB coverage | Less (each entry covers less memory) | More (each entry covers more) |
| Disk I/O granularity | More seeks per transfer | Fewer seeks per transfer |

4096 bytes has been the sweet spot since the VAX in the 1970s. In
practice, sub-4 KB pages don't exist on any modern architecture — the
"smaller pages" column is purely hypothetical. If anything, the
pressure today is in the *other* direction: RAM sizes have grown from
megabytes to hundreds of gigabytes, making 4 KB pages expensive to
manage (a 256 GB server needs 67 million page table entries at 4 KB
each), which is why huge pages (2 MB, 1 GB) are increasingly
important. The RISC-V Sv39 page table format is designed around 4 KB
pages (12 bits of page offset, since 2^12 = 4096). We'll see this in
Phase 4.

> **Larger page sizes exist.** RISC-V Sv39 supports 2 MB "megapages"
> and 1 GB "gigapages" for mapping large contiguous regions efficiently
> (fewer TLB entries). Linux calls these "huge pages." We won't use
> them, but the page table hardware supports them natively.
>
> Huge pages matter in practice because the TLB is small (typically
> 64-1024 entries). With 4 KB pages, 1024 TLB entries cover only 4 MB.
> With 2 MB huge pages, the same 1024 entries cover 2 GB. For any
> workload that touches gigabytes of data with unpredictable access
> patterns (databases, VMs, packet processing), reducing TLB misses
> is a measurable performance win.
>
> All major OSes support huge pages, but the mechanisms differ:
>
> | OS | Mechanism | How to configure |
> |----|-----------|-----------------|
> | **Linux** | **Explicit huge pages:** pre-reserve a pool at boot (`vm.nr_hugepages=1024` in sysctl, or `hugepages=1024` on kernel command line). Apps request them via `mmap` with `MAP_HUGETLB` or by mounting `hugetlbfs`. | `sysctl vm.nr_hugepages=1024` |
> | | **Transparent Huge Pages (THP):** the kernel automatically promotes contiguous 4 KB pages to 2 MB pages in the background, no app changes needed. Enabled by default on most distros. Downside: the background defragmentation (`khugepaged` thread) can cause latency spikes — Redis famously recommends disabling THP for this reason. | `echo always > /sys/kernel/mm/transparent_hugepage/enabled` |
> | **Windows** | Called "Large Pages." Requires the `SeLockMemoryPrivilege` privilege. Apps request them via `VirtualAlloc` with `MEM_LARGE_PAGES`. SQL Server and JVMs use this. | Group Policy → Lock Pages in Memory |
> | **macOS** | Called "superpage" support. Apps request via `mmap` with `VM_FLAGS_SUPERPAGE_SIZE_2MB`. Less commonly used than on Linux/Windows. | Per-process via `mmap` flags |
> | **FreeBSD** | Supports both explicit ("superpage reservations") and transparent promotion. Similar to Linux's dual approach. | `sysctl vm.pmap.pg_ps_enabled=1` |
>
> The kernel itself also uses huge pages internally — Linux maps the
> kernel text and the direct-map region (all of physical memory mapped
> at a fixed offset) with 2 MB or 1 GB pages to minimize TLB pressure
> during kernel execution.

### Page-aligned addresses

An address is **page-aligned** if it's a multiple of 4096 (i.e., its
low 12 bits are zero):

```
Page-aligned:     0x80000000, 0x80001000, 0x80002000, 0x88000000
Not page-aligned: 0x80000001, 0x80000FFF, 0x8000A234
```

Page alignment matters because:

1. **Page tables require it.** A page table entry (PTE) stores a
   physical page number, not a full address. The low 12 bits of the
   physical address are always zero (page-aligned), so the PTE doesn't
   need to store them — it uses those bits for permission flags
   instead (R, W, X, U, etc.). We'll study PTEs in detail in Phase 4,
   but the trick should look familiar: it's the same "steal
   known-zero bits for metadata" pattern we saw in PMP's NAPOT mode,
   where the low bits of `pmpaddr` encode the region size instead of
   address bits (Lecture 2-1).

2. **The allocator requires it.** `kalloc()` always returns a
   page-aligned address. `kfree()` only accepts page-aligned
   addresses. This simplifies bookkeeping — every allocated block is
   the same size (4096 bytes) and starts at a predictable boundary.

3. **Hardware requires it for some structures.** The `satp` register
   (page table root pointer) stores a physical page number, not a byte
   address. The page table itself must be page-aligned. Trap vector
   addresses (`stvec`) must be 4-byte aligned (which page alignment
   trivially satisfies).

### The alignment macros

Two macros are used throughout any OS kernel:

**PGROUNDDOWN** — round an address *down* to the nearest page boundary:

```c
#define PGROUNDDOWN(addr) ((addr) & ~(PGSIZE - 1))
```

This clears the low 12 bits. Examples:

```
PGROUNDDOWN(0x80001234) = 0x80001000
PGROUNDDOWN(0x80001000) = 0x80001000  (already aligned — no change)
PGROUNDDOWN(0x80000FFF) = 0x80000000
```

**PGROUNDUP** — round an address *up* to the nearest page boundary:

```c
#define PGROUNDUP(addr)   (((addr) + PGSIZE - 1) & ~(PGSIZE - 1))
```

This adds `PGSIZE - 1` (4095) and then clears the low 12 bits. The
addition ensures that any non-aligned address gets bumped to the next
boundary. Examples:

```
PGROUNDUP(0x80001234) = 0x80002000
PGROUNDUP(0x80001000) = 0x80001000  (already aligned — no change)
PGROUNDUP(0x80000001) = 0x80001000
```

> **The math behind PGROUNDUP.** Let's trace through
> `PGROUNDUP(0x80001234)` step by step:
>
> ```
> addr               = 0x80001234
> PGSIZE - 1         = 0x00000FFF
> addr + PGSIZE - 1  = 0x80001234 + 0x00000FFF = 0x80002233
> ~(PGSIZE - 1)      = 0xFFFFFFFFFFFFF000
> result & mask       = 0x80002233 & 0xFFFFFFFFFFFFF000 = 0x80002000
> ```
>
> The `+ PGSIZE - 1` is the standard "round up by adding (unit - 1)
> before truncating" trick. It's the same logic as integer ceiling
> division: `ceil(a/b) = (a + b - 1) / b`. Here, we're doing
> `ceil(addr / PGSIZE) * PGSIZE`.
>
> Why `PGSIZE - 1` and not `PGSIZE`? If `addr` is already aligned,
> adding `PGSIZE` would overshoot by one page. Adding `PGSIZE - 1`
> keeps it in place:
>
> ```
> PGROUNDUP(0x80001000):
>   0x80001000 + 0xFFF = 0x80001FFF
>   0x80001FFF & 0xFFFFFFFFFFFFF000 = 0x80001000  (correct — no change)
>
> If we used PGSIZE instead of PGSIZE-1:
>   0x80001000 + 0x1000 = 0x80002000
>   0x80002000 & 0xFFFFFFFFFFFFF000 = 0x80002000  (WRONG — jumped one page)
> ```

### Splitting an address into page number and offset

Any physical address can be decomposed:

```
Physical address:    0x 80001 234
                        |---| |-|
                         PPN  offset (12 bits)

  PPN    = upper bits   = 0x80001234 >> 12    = 0x80001
  offset = low 12 bits  = 0x80001234 & 0xFFF  = 0x234

  Reconstruct: (0x80001 << 12) | 0x234 = 0x80001234
```

This decomposition is the foundation of the page table, which we'll
build in Phase 4. The page table maps virtual page numbers to physical
page numbers. The 12-bit offset passes through untranslated.

For now, the allocator only cares about page-aligned addresses (where
offset = 0), but understanding the split helps you see *why* 4096 is
special: it's 2^12, which gives exactly 12 bits of offset.

---

## Part 6: xv6's `memlayout.h`

### What xv6 defines

xv6 puts all platform address constants in one header,
`kernel/memlayout.h`. Here's the full file (simplified slightly):

```c
// kernel/memlayout.h (xv6)

// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
//   80000000 -- entry.S, then kernel text and data
//   end -- start of kernel page allocation area
//   PHYSTOP -- end of RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio interface
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// core local interruptor (CLINT), which contains the timer.
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // cycles since boot

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L

#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)
```

(xv6's `memlayout.h` also defines virtual address constants like
`TRAMPOLINE`, `KSTACK`, `TRAPFRAME` — those are for the virtual
memory layout and we'll add them in Phase 4.)

### What we need now vs. later

Some of these constants we already have (in `riscv.h` or `uart.c`).
Some we need to add. Some we don't need yet:

| Constant | xv6 location | bobchouOS status | Need now? |
|----------|-------------|-----------------|:---------:|
| `UART0` (0x10000000) | `memlayout.h` | Hardcoded in `uart.c` | Move to header |
| `CLINT` / `CLINT_MTIME` / `CLINT_MTIMECMP` | `memlayout.h` | Already in `riscv.h` | Keep where it is |
| `VIRTIO0` (0x10001000) | `memlayout.h` | Not defined | No (Phase 7+) |
| `PLIC` (0x0C000000) | `memlayout.h` | Not defined | No (Phase 5+) |
| `KERNBASE` (0x80000000) | `memlayout.h` | Implicit in `linker.ld` | **Yes** |
| `PHYSTOP` (0x88000000) | `memlayout.h` | Not defined | **Yes** |
| `PGSIZE` (4096) | `riscv.h` | Not defined | **Yes** |
| `PGROUNDUP` / `PGROUNDDOWN` | `riscv.h` | Not defined | **Yes** |

### Where to put what

xv6 puts `PGSIZE` and alignment macros in `riscv.h` (they're
architecture-specific — the page size is defined by the RISC-V Sv39
spec). Platform addresses go in `mem_layout.h` (they're
machine-specific — a different board would have different addresses).

We'll follow the same split:

- **`mem_layout.h`** (new file): `KERNBASE`, `PHYSTOP`, `UART0` (moved
  from `uart.c`), and any future platform addresses
- **`riscv.h`** (updated): `PGSIZE`, `PGROUNDUP`, `PGROUNDDOWN`

The CLINT constants stay in `riscv.h` for now — they're used by
`entry.S` (assembly), and `riscv.h` is already included there. We
could move them to `mem_layout.h` later, but there's no pressing reason
to reorganize.

> **Why not put everything in one header?**
>
> Separation of concerns. `riscv.h` defines things that come from the
> RISC-V ISA specification — CSR bit positions, page sizes, privilege
> mode constants. These are the same on every RISC-V machine.
> `mem_layout.h` defines things that come from the *platform* — where
> QEMU's `virt` machine puts its devices and RAM. A different board
> (SiFive HiFive) would have a different `mem_layout.h` but the same
> `riscv.h`.
>
> In practice, the distinction blurs (is CLINT an ISA thing or a
> platform thing? Answer: it's a platform thing, but we've already put
> it in `riscv.h` and assembly depends on it). Don't stress about
> perfect separation — the important thing is that `KERNBASE` and
> `PHYSTOP` are named constants in a header, not magic numbers buried
> in code.

---

## Part 7: Naming Conventions and Types

### Address types: pointer vs. integer

One thing you'll notice in xv6's allocator (and in our code) is a lot
of casting between `void *` (pointer) and `uint64` (integer). This
isn't sloppy code — it reflects a real duality:

- **Addresses as pointers** — when you want to *read or write* the
  memory: `*(uint64 *)addr = value;`
- **Addresses as integers** — when you want to *do arithmetic* on the
  address: `addr + PGSIZE`, `addr & ~0xFFF`, `addr >= KERNBASE`

C doesn't let you do arithmetic on `void *` (it's undefined behavior
in standard C). You can't write `void *next = ptr + 4096;` — the
compiler doesn't know the element size. So when you need arithmetic,
you cast to `uint64`, do the math, and cast back to a pointer:

```c
void *next_page = (void *)((uint64)ptr + PGSIZE);
```

xv6's `kalloc.c` is full of these casts. The xv6 book explains:

> *"The allocator sometimes treats addresses as integers in order to
> perform arithmetic on them (e.g., traversing all pages in
> freerange), and sometimes uses addresses as pointers to read and
> write memory (e.g., manipulating the run structure stored in each
> page); this dual use of addresses is the main reason that the
> allocator code is full of C type casts."*

We'll see this in Round 3-2 when we write the allocator. For now,
just know that `uint64` and `char *` and `void *` are all different C
views of the same thing: a 64-bit number that identifies a memory
location.

> **`void *` — implicit conversion and arithmetic (a Lecture 0-3
> update).** In the C recap, we wrote `int *specific = (int *)generic;`
> with an explicit cast. That was the safe teaching choice, but in C
> (not C++), the cast is actually optional. `void *` converts
> implicitly to and from any pointer type on **assignment**:
>
> ```c
> void *v = &x;          // int* → void*: implicit, no cast needed
> int *p = v;            // void* → int*: implicit in C, error in C++
> struct run *r = v;     // void* → struct run*: also implicit in C
> ```
>
> This is why `malloc()` doesn't need a cast in C:
> `int *p = malloc(40);` works because `malloc` returns `void *` and
> C converts it silently.
>
> But **pointer arithmetic** is a different story — it depends on the
> pointed-to type's size:
>
> ```c
> int *p   = (int *)0x1000;    p + 1;  // 0x1004 (+4 bytes)
> char *c  = (char *)0x1000;   c + 1;  // 0x1001 (+1 byte)
> struct run *r = ...;         r + 1;  // +8 bytes (one pointer)
> void *v  = (void *)0x1000;   v + 1;  // illegal in standard C
> ```
>
> The type doesn't change the stored bit pattern — all pointers are 8
> bytes on rv64. It only affects how `+1` is interpreted. This is why
> the compiler cares about pointer types at all.
>
> For `void *`, arithmetic is undefined in standard C because
> `sizeof(void)` is undefined. But GCC and Clang both allow it as an
> extension, treating `void *` like `char *` (byte-granularity). So
> `(void *)0x1000 + 4` gives `(void *)0x1004`. xv6 and most kernel
> code relies on this (kernel code is always compiled with GCC or
> Clang, never MSVC). We could rely on it too, but explicit casts make
> the intent clearer and avoid `-Wpedantic` warnings.
>
> **GCC vs. Clang — a quick primer.** Both are open-source C/C++
> compilers, and for kernel code they produce nearly identical output.
> But they have very different origins:
>
> - **GCC** (GNU Compiler Collection, 1987) — the GNU project's
>   compiler, licensed under **GPL**. If you modify GCC and distribute
>   it, you must release your changes. Default compiler on most Linux
>   distros, and supports the most targets of any compiler (x86, ARM,
>   RISC-V, MIPS, PowerPC, SPARC, and dozens of niche architectures
>   that LLVM doesn't cover). Monolithic architecture — hard to reuse
>   as a library.
> - **Clang/LLVM** (2003, Chris Lattner at UIUC, later backed by
>   Apple) — LLVM originally stood for "Low Level Virtual Machine,"
>   but the project outgrew the name and today it's officially not an
>   acronym. Licensed under **Apache 2.0** (permissive). Designed as a
>   modular library from the start: Clang is the C/C++ frontend, LLVM
>   is the optimizer and backend. You can embed LLVM in your own tools.
>   Default compiler on macOS, FreeBSD, and Android NDK.
>
> Apple was the key driver behind Clang/LLVM's growth. They wanted to
> build proprietary compiler features (Objective-C improvements, GPU
> shader compilers) without being forced to open-source them under GPL.
> LLVM's permissive license allowed that. Apple replaced GCC with Clang
> in Xcode around 2012 and never looked back.
>
> The bigger impact of LLVM goes far beyond C. To understand why,
> let's look at what a compiler actually does:
>
> ```
> Traditional compiler (e.g., GCC):
>
>   Source code → [Frontend] → [Optimizer] → [Backend] → Machine code
>                 (parse,       (constant      (register
>                  type-check)   folding,       allocation,
>                                inlining,      instruction
>                                dead code      selection,
>                                removal)       emit .o file)
> ```
>
> In GCC, these stages are tightly coupled — GCC's optimizer and
> backend only understand GCC's internal data structures. You can't
> plug in a different frontend and reuse GCC's optimizer.
>
> LLVM's key insight was to define a **clean intermediate
> representation (IR)** between the frontend and backend:
>
> ```
> LLVM architecture:
>
>   Source code → [Frontend] → LLVM IR → [Optimizer] → [Backend] → Machine code
>                 (Clang,       ^          (shared!)     (x86, ARM,
>                  rustc,       |                         RISC-V,
>                  swiftc,      |                         WebAssembly...)
>                  ...)         |
>                        "universal assembly language"
>                        — typed, SSA form, target-independent
> ```
>
> LLVM IR is a well-defined, target-independent language — think of
> it as a "universal assembly language" that any frontend can emit and
> any backend can consume. The optimizer works entirely on IR, so its
> improvements (inlining, vectorization, dead code removal) benefit
> *every* language that targets LLVM, not just C.
>
> Because LLVM is a modular library, any language can use it as a
> backend: compile your language to LLVM IR, and LLVM handles
> optimization and machine code generation for every target it
> supports. This is exactly what happened:
>
> | Language | Frontend | Backend |
> |----------|----------|---------|
> | C / C++ | Clang | LLVM |
> | **Rust** | `rustc` | LLVM |
> | **Swift** | `swiftc` (Apple) | LLVM |
> | **Julia** | Julia parser | LLVM (JIT) |
> | **Zig** | Zig compiler | LLVM (also has a self-hosted backend) |
> | **Kotlin Native** | Kotlin compiler | LLVM |
>
> When you compile Rust code, `rustc` parses it, does borrow checking
> and type checking, then emits LLVM IR. From that point on, the exact
> same LLVM optimizer and code generator that Clang uses turns it into
> machine code. This is why Rust can target every platform LLVM
> supports (x86, ARM, RISC-V, WebAssembly, ...) without writing a
> separate code generator for each one.
>
> **In day-to-day use,** Clang generally produces better error
> messages than GCC — more precise source locations, color-highlighted
> ranges, and "did you mean X?" suggestions. Code generation quality
> is roughly equal: GCC is sometimes better at auto-vectorization,
> Clang is sometimes better at inlining decisions. For kernel code
> the difference is negligible.
>
> For bobchouOS, we use `riscv-none-elf-gcc` because that's the
> standard RISC-V bare-metal toolchain. The Linux kernel supported
> only GCC for decades; Clang support was added around 2018 and is now
> fully supported — Android kernels are built with Clang by default.

### The `extern char[]` trick for linker symbols

In `main.c`, we declare linker symbols like this:

```c
extern char _kernel_start[];
extern char _kernel_end[];
```

Why `char[]` and not `char *`? Because linker symbols aren't
variables — they don't occupy memory. The linker assigns an *address*
to the symbol, but there's no pointer variable stored anywhere. If you
wrote `extern char *_kernel_end;`, the compiler would try to load a
pointer value from the address of `_kernel_end` — but there's no
pointer stored there, just whatever bytes happen to follow the BSS
section. You'd get garbage.

With `extern char _kernel_end[];`, the expression `_kernel_end` in C
evaluates to the *address of the symbol* (array names decay to
pointers). No memory load happens — the linker directly substitutes
the address. This is the standard idiom for accessing linker-provided
symbols in C.

To use it as an integer for arithmetic:

```c
uint64 free_start = PGROUNDUP((uint64)_kernel_end);
```

Cast the `char *` (array decayed to pointer) to `uint64`, round up to
a page boundary, and you have the first allocatable address.

---

## Part 8: Constants We're Defining

### Summary of new definitions

Here's everything Round 3-1 introduces:

**In `riscv.h`** (architecture constants):

```c
#define PGSIZE      4096            // bytes per page
#define PGSHIFT     12              // bits of offset within a page

#define PGROUNDUP(a)   (((a) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a)  ((a) & ~(PGSIZE - 1))
```

`PGSHIFT` (12) is the number of bits you shift to convert between a
byte address and a page number. We don't need it for the allocator,
but it's used extensively in page table code (Phase 4). Defining it
now keeps related constants together.

**In `mem_layout.h`** (new file, platform constants):

```c
#define KERNBASE    0x80000000UL    // start of DRAM
#define PHYSTOP     (KERNBASE + 128 * 1024 * 1024)  // end of DRAM

#define UART0_BASE  0x10000000UL    // 16550 UART (moved from uart.c)
```

We rename `UART0` to `UART0_BASE` to be more descriptive (it's a base
address, not a device object). The `uart.c` file will `#include
"mem_layout.h"` and use `UART0_BASE` instead of its local `#define`.

### What about CLINT and PLIC?

The CLINT constants (`CLINT_BASE`, `CLINT_MTIMECMP`, `CLINT_MTIME`)
stay in `riscv.h` for now. They're used by `entry.S` (which includes
`riscv.h` but shouldn't include `mem_layout.h` — different concern),
and they work fine where they are.

The PLIC (`0x0C000000`) isn't defined anywhere yet. We don't need it
until we add external interrupt handling (probably Phase 5 or later).
We'll add it to `mem_layout.h` when the time comes.

### The complete memory picture with named constants

After Round 3-1, we can describe the entire physical address space
using named constants:

```
0x00000000  +---------------------+
            |  (unmapped/debug)   |
0x02000000  +---------------------+  CLINT_BASE
            |  CLINT (timer, IPI) |
0x0C000000  +---------------------+  (future: PLIC_BASE)
            |  PLIC (ext. irqs)   |
0x10000000  +---------------------+  UART0_BASE
            |  UART0 (serial)     |
0x10001000  +---------------------+  (future: VIRTIO0_BASE)
            |  virtio disk        |
            |       ...           |
0x80000000  +=====================+  KERNBASE / _kernel_start
            |  .text              |
            |  .rodata            |
            |  .data              |
            |  .bss               |
            |  stack (16 KB)      |
            +---------------------+  _kernel_end
            |                     |
            |  FREE PAGES         |  <-- kalloc() hands these out
            |  (allocatable)      |
            |                     |
0x88000000  +=====================+  PHYSTOP
```

Every address in this diagram is now either a linker symbol or a
`#define` constant. No magic numbers in kernel code.

---

## Part 9: How bobchouOS Will Do It

### The plan

Round 3-1 is code-light. We're creating one new file and updating two
existing ones:

**New: `kernel/include/mem_layout.h`**
- `KERNBASE` — start of DRAM (0x80000000)
- `PHYSTOP` — end of DRAM (KERNBASE + 128 MB)
- `UART0_BASE` — UART base address (moved from `uart.c`)

**Updated: `kernel/include/riscv.h`**
- `PGSIZE` — page size (4096)
- `PGSHIFT` — page offset bits (12)
- `PGROUNDUP(a)` — round up to page boundary
- `PGROUNDDOWN(a)` — round down to page boundary

**Updated: `kernel/drivers/uart.c`**
- Remove local `#define UART0` and use `UART0_BASE` from `mem_layout.h`

### File layout

```
kernel/
    include/
        mem_layout.h    <-- NEW (platform address constants)
        riscv.h         <-- UPDATE (add PGSIZE, PGSHIFT, PGROUND macros)
        types.h         <-- unchanged
    drivers/
        uart.c          <-- UPDATE (use UART0_BASE from mem_layout.h)
    main.c              <-- unchanged
    trap.c              <-- unchanged
```

### What doesn't change

- `linker.ld` — already defines `_kernel_start` and `_kernel_end`.
  No changes needed. We'll add a comment updating the ASCII diagram
  to show the free region above `_kernel_end`.
- `entry.S` — uses CLINT constants from `riscv.h`. Unaffected.
- `trap.c` — no memory-related changes.
- `main.c` — no changes in this round (Round 3-2 will add `kinit()`).

### Comparison with xv6

| Aspect | xv6 | bobchouOS (Round 3-1) |
|--------|-----|----------------------|
| Platform constants file | `memlayout.h` | `mem_layout.h` (same idea) |
| Page size / alignment macros | In `riscv.h` | In `riscv.h` (same) |
| UART address | In `memlayout.h` | In `mem_layout.h` (moved from `uart.c`) |
| CLINT address | In `memlayout.h` | In `riscv.h` (stays — used by assembly) |
| `KERNBASE` value | `0x80000000L` | `0x80000000UL` (unsigned — avoids signed overflow) |
| `PHYSTOP` calculation | `KERNBASE + 128*1024*1024` | Same formula |
| RAM size | Hardcoded 128 MB | Hardcoded 128 MB (same) |
| Device tree parsing | No | No (same) |

The main differences are cosmetic: we use `UL` suffix (unsigned long)
instead of `L` (signed long) to avoid potential signed arithmetic
issues with addresses above 0x7FFFFFFF, and we name the UART constant
`UART0_BASE` instead of `UART0`.

---

## Part 10: Looking Ahead — What the Allocator Will Need

### The Round 3-2 preview

With the constants from Round 3-1 in place, Round 3-2 will build the
actual page allocator. Here's a preview of what it needs:

```c
// kernel/kalloc.c (Round 3-2 preview)

struct run {
    struct run *next;
};

struct run *freelist;     // head of the free list

void kinit(void) {
    // Free every page from _kernel_end (rounded up) to PHYSTOP
    freerange((void *)PGROUNDUP((uint64)_kernel_end), (void *)PHYSTOP);
}

void *kalloc(void) {
    // Pop a page from the free list
    struct run *r = freelist;
    if (r)
        freelist = r->next;
    // Zero the page before returning (clean slate for page tables)
    if (r)
        memset((char *)r, 0, PGSIZE);
    return (void *)r;
}

void kfree(void *pa) {
    // Push a page onto the free list
    memset(pa, 1, PGSIZE);           // fill with junk (catch dangling refs)
    struct run *r = (struct run *)pa;
    r->next = freelist;
    freelist = r;
}
```

Notice how every constant from Round 3-1 appears:
- `PGROUNDUP` — to align the start of free memory
- `PHYSTOP` — to mark the end of free memory
- `PGSIZE` — to know how much to zero/junk-fill per page
- `_kernel_end` — to find where the kernel stops

The allocator stores its linked list node (`struct run`) *inside the
free page itself*. A free 4096-byte page has nothing useful in it, so
we reuse the first 8 bytes as a `next` pointer. When the page is
allocated and returned to the caller, those bytes become part of the
page's payload — the caller overwrites them with whatever data they
want. This "embed metadata in the free block" trick is used in xv6,
Linux's page allocator, `malloc()` implementations, and most free-list
allocators.

We'll explain all of this in detail in the Round 3-2 lecture. For now,
just notice that the constants and macros we're defining in Round 3-1
are the vocabulary that makes the allocator code readable.

---

## What's Next

After you read this lecture, we'll:

1. **Create the skeleton** — write `mem_layout.h`, add page macros to
   `riscv.h`, and update `uart.c` to use the shared constant. These
   changes are small, so the skeleton will be close to complete (not
   much to fill in as TODOs — the "implementation" is mostly picking
   the right values for the constants).

2. **You verify the build** — `make run` should produce the same
   output as before. No behavior changes in this round — we're only
   adding constants and reorganizing an existing one.

3. **Round 3-2** — the real implementation round: `kalloc.c`,
   `kfree()`, `kinit()`, free list, tests. That's where the allocated
   page count shows up in the boot banner and we can finally say the
   kernel manages memory.

---

## Quick Reference

### Physical address map (QEMU `virt` machine)

| Address | Size | Region |
|---------|------|--------|
| `0x0000_1000` | 0x100 | Boot ROM |
| `0x0200_0000` | 0x10000 | CLINT (timer, software interrupts) |
| `0x0C00_0000` | 0x600000 | PLIC (external interrupts) |
| `0x1000_0000` | 0x100 | UART0 (serial port) |
| `0x1000_1000` | 0x1000 | virtio disk |
| `0x8000_0000` | 0x0800_0000 | DRAM (128 MB default) |

### Named constants (after Round 3-1)

| Constant | Value | Defined in | Purpose |
|----------|-------|-----------|---------|
| `KERNBASE` | `0x80000000` | `mem_layout.h` | Start of DRAM |
| `PHYSTOP` | `0x88000000` | `mem_layout.h` | End of DRAM (KERNBASE + 128 MB) |
| `UART0_BASE` | `0x10000000` | `mem_layout.h` | UART device base address |
| `PGSIZE` | `4096` | `riscv.h` | Bytes per page |
| `PGSHIFT` | `12` | `riscv.h` | Bits of page offset (log2 of PGSIZE) |
| `PGROUNDUP(a)` | — | `riscv.h` | Round up to page boundary |
| `PGROUNDDOWN(a)` | — | `riscv.h` | Round down to page boundary |

### Linker symbols (from `linker.ld`)

| Symbol | Meaning |
|--------|---------|
| `_kernel_start` | First byte of kernel image (= KERNBASE) |
| `_text_end` | End of `.text` section (page-aligned) |
| `_bss_start` | Start of `.bss` section |
| `_bss_end` | End of `.bss` section |
| `_kernel_end` | Last byte of kernel image (after stack) |

### Alignment macros

```c
// Round down to page boundary (clear low 12 bits)
PGROUNDDOWN(0x80001234) = 0x80001000

// Round up to page boundary (bump then clear low 12 bits)
PGROUNDUP(0x80001234)   = 0x80002000
PGROUNDUP(0x80001000)   = 0x80001000  // already aligned — no change

// Decompose address into page number and offset
PPN    = addr >> PGSHIFT       // = addr >> 12
offset = addr & (PGSIZE - 1)  // = addr & 0xFFF
```

### Free memory region

```
Start:  PGROUNDUP((uint64)_kernel_end)
End:    PHYSTOP
Size:   ~128 MB minus kernel image size
Unit:   4096-byte pages
```
