# Lecture 2-3: Timer Interrupts and the CLINT

> **Where we are**
>
> The kernel boots into S-mode, and we have a working trap infrastructure.
> `kernel_vec` in `kernel_vec.S` saves caller-saved registers, `kernel_trap()`
> in `trap.c` reads `scause` and prints diagnostics, and `stvec` is wired
> up in `kmain()`. We verified everything by triggering an illegal
> instruction — `csrr mhartid` from S-mode causes exception 2, and our
> handler catches it cleanly.
>
> But our kernel is static. It boots, prints some text, triggers a test
> trap, and halts. A real OS needs a **heartbeat** — a periodic interrupt
> that fires at regular intervals, giving the kernel a chance to do
> housekeeping: switch between processes, update timers, check deadlines.
> That heartbeat is the **timer interrupt**.
>
> This lecture explains how RISC-V timer interrupts work, from the
> hardware that generates them (the CLINT) to the software chain that
> delivers them to S-mode. The core challenge: RISC-V timer interrupts
> are *machine-mode only* — they cannot be delegated to S-mode. So we
> need a small M-mode handler that catches each tick and forwards it to
> S-mode as a software interrupt. This is the trickiest piece of our
> entire trap infrastructure.
>
> By the end of this lecture, you will understand:
>
> - What the CLINT is and how it generates timer interrupts
> - The `mtime` and `mtimecmp` registers — the timer hardware interface
> - Why machine timer interrupts (MTI) cannot be delegated to S-mode
> - The M-mode forwarding trick: catch MTI, raise SSIP for S-mode
> - How to write a tiny M-mode assembly handler (`timer_vec`)
> - The `mscratch` scratch area trick for saving registers in M-mode
> - The `mie`, `sie`, and `sstatus.SIE` enable bits and how they gate
>   interrupts at each level
> - How to count timer ticks and print periodic output
> - How xv6 implements all of this (`timerinit` + `timervec`)
> - How bobchouOS will do the same
>
> **xv6 book coverage:** This lecture absorbs Chapter 5, section 5.4
> (Timer interrupts). It also touches on section 5.5 (Real world) for
> context on how timer interrupts relate to scheduling and preemption.
> The scheduling details (calling `yield()` on timer ticks) wait until
> Phase 5.

---

## Part 1: Why Timer Interrupts?

### The need for a heartbeat

Right now, our kernel boots, prints some output, and halts. If we added
more features — a shell, user programs, disk I/O — the kernel would
still need a way to *regain control* from running code periodically.

Consider a user program stuck in an infinite loop:

```
while (1) {
    // spinning forever, never making a system call
}
```

Without a timer interrupt, the kernel would never run again. The CPU
belongs to that program forever. No other program gets a turn, no
deadlines are checked, no timeouts fire. The system is stuck.

Timer interrupts solve this. Every N microseconds, the timer hardware
fires an interrupt. The CPU stops whatever it's doing (even an infinite
loop), traps into the kernel, and the kernel gets to decide what to do
next: schedule another process, update the system clock, check if a
sleeping process should wake up.

This is called **preemptive multitasking** — the kernel *preempts*
(forcibly stops) running code, rather than waiting for it to
voluntarily yield. Every modern OS uses it.

### What we'll build in Round 2-3

We don't have processes yet (that's Phase 5), so we won't preempt
anything. Instead, we'll:

1. Program the timer hardware to fire every 10ms (100 ticks/second)
2. Count the ticks in a global variable
3. Print a message every 100 ticks (once per second) to prove it works
4. Let `kmain()` spin in a loop while the timer keeps firing

This gives us the timer infrastructure that scheduling will plug into
later. The plumbing is the hard part — once it works, adding `yield()`
calls in Phase 5 is straightforward.

---

## Part 2: The CLINT — Core Local Interruptor

### What is the CLINT?

The **CLINT** (Core Local Interruptor) is a hardware block inside the
SoC that provides two things to each hart (hardware thread):

1. **Timer functionality** — a shared time counter and per-hart
   deadline comparators
2. **Software interrupts** — a way for one hart to poke another hart
   (inter-processor interrupt, or IPI)

The name "Core Local" means the interrupts go directly to specific
harts — unlike the PLIC (Platform-Level Interrupt Controller) which
routes external device interrupts and involves priority arbitration.
The CLINT is simpler: timer and software, per-hart, no priority logic.

```
                   SoC
  +--------------------------------------------+
  |                                            |
  |   +------+  +------+  +------+             |
  |   | Hart |  | Hart |  | Hart |  ...        |
  |   |  0   |  |  1   |  |  2   |             |
  |   +--+---+  +--+---+  +--+---+             |
  |      |         |         |                 |
  |   +--+---------+---------+---+             |
  |   |          CLINT           |             |
  |   |  mtime (shared counter)  |             |
  |   |  mtimecmp[0] (hart 0)    |             |
  |   |  mtimecmp[1] (hart 1)    |             |
  |   |  mtimecmp[2] (hart 2)    |             |
  |   |  msip[0..N]  (IPI regs)  |             |
  |   +--------------------------+             |
  |                                            |
  |   +--------------------------+             |
  |   |          PLIC            | (for UART,  |
  |   |  (external interrupts)   |  disk, etc) |
  |   +--------------------------+             |
  +--------------------------------------------+
```

> **CLINT vs PLIC — when to use which?**
>
> | Feature | CLINT | PLIC |
> |---------|-------|------|
> | What it handles | Timer + software interrupts | External device interrupts |
> | Scope | Per-hart (each hart has own comparator) | Global (routes to any hart) |
> | Priority logic | None (simple compare/flag) | Yes (configurable priorities) |
> | Interrupt type | Machine-mode only | Can target S-mode |
> | Example sources | Timer deadline, IPI | UART, disk, network |
>
> We'll deal with the PLIC when we add UART interrupts (a later round).
> For now, the CLINT is all we need.

### CLINT memory map

The CLINT is a **memory-mapped** device — you read and write its
registers by accessing specific physical addresses. On QEMU's `virt`
machine, the CLINT lives at base address `0x2000000`:

```
Address               Register        Size     Description
--------------------  --------------  -------  ---------------------------
0x200_0000            msip[0]         4 bytes  Software interrupt for hart 0
0x200_0004            msip[1]         4 bytes  Software interrupt for hart 1
  ...                   ...
0x200_4000            mtimecmp[0]     8 bytes  Timer compare for hart 0
0x200_4008            mtimecmp[1]     8 bytes  Timer compare for hart 1
  ...                   ...
0x200_BFF8            mtime           8 bytes  Global time counter
```

> **Why is `mtime` way down at `0xBFF8`?** The CLINT register layout
> follows the SiFive CLINT specification, which pre-dates the formal
> RISC-V Advanced Interrupt Architecture (AIA). The regions are laid
> out sequentially, each sized for up to **4,096 harts**:
>
> ```
> 0x0000 - 0x3FFF   msip[0..4095]       4,096 x 4 bytes = 16 KB
> 0x4000 - 0xBFF7   mtimecmp[0..4095]   4,096 x 8 bytes = 32 KB
> 0xBFF8 - 0xBFFF   mtime               1 x 8 bytes
> ```
>
> That's why `mtimecmp` starts at `0x4000` (after 4,096 `msip` slots)
> and `mtime` sits at `0xBFF8` (after 4,096 `mtimecmp` slots). No real
> chip has 4,096 harts today (the largest RISC-V chips have around 64
> cores), but the address space reservation means the layout won't need
> to change if someone builds a massive many-core chip in the future.

### The three register types

**`mtime`** (64-bit, read-only from software's perspective):

A free-running counter that increments at a fixed frequency. On QEMU's
`virt` machine, it increments at **10 MHz** (10,000,000 ticks per
second). This counter never stops and never wraps in any practical
timeframe (2^64 ticks at 10 MHz = ~58,000 years).

Think of `mtime` as a wall clock. It always tells you the current time,
and you can't change it (well, technically you can write to it, but you
shouldn't — it's shared across all harts).

> **How does 10 MHz compare to other platforms?** Every architecture has
> a similar free-running timer counter, but the frequencies vary wildly:
>
> | Platform | Timer frequency | Timer type |
> |----------|----------------|------------|
> | QEMU RISC-V `virt` | 10 MHz | CLINT `mtime` |
> | HiFive Unmatched (real RISC-V) | 1 MHz | CLINT `mtime` |
> | Raspberry Pi 4 (ARM Cortex-A72) | 54 MHz | ARM Generic Timer |
> | AWS Graviton 4 (ARM Neoverse V2) | 1,000 MHz (1 GHz) | ARM Generic Timer |
> | Typical x86 (Intel/AMD) | ~1-3.5 GHz | TSC (Time Stamp Counter) |
>
> The ARM Generic Timer (`CNTPCT_EL0` / `CNTP_CVAL_EL0`) is
> conceptually the same as RISC-V's `mtime`/`mtimecmp` — a free-running
> counter and a compare register that fires an interrupt when the
> deadline is reached. Different register names, different access
> mechanisms (system registers instead of MMIO), but the same pattern.
> Real RISC-V hardware tends to use slower timer clocks (1 MHz is
> common) because `mtime` is meant to be a real-time clock, not a
> high-resolution performance counter.

**`mtimecmp[hart]`** (64-bit, read-write, per-hart):

A deadline register. When `mtime >= mtimecmp[hart]`, the CLINT asserts
the **machine timer interrupt** (MTI) for that hart. The interrupt stays
asserted as long as `mtime >= mtimecmp`. To clear it, software writes a
new (future) value to `mtimecmp`.

This is the key mechanism: you write "fire at time X" to `mtimecmp`,
and when the clock reaches X, you get an interrupt. To get periodic
interrupts, the handler reads `mtimecmp`, adds an interval, and writes
it back:

```
mtimecmp = mtimecmp + interval;   // next deadline
```

> **Why add to `mtimecmp` instead of `mtime + interval`?**
>
> If we wrote `mtimecmp = mtime + interval`, the ticks would slowly
> drift. Each tick arrives slightly late (the handler takes some time to
> run), and `mtime` has already advanced past the ideal next deadline by
> the time we read it. Over many ticks, the error accumulates.
>
> By adding to `mtimecmp` (the *scheduled* deadline, not the *actual*
> current time), we get precise tick spacing. If a tick is late, the next
> deadline is still calculated from where it *should have been*, not from
> where it actually was. This is the same idea as using a fixed-rate
> timer in game loops: `next_frame_time += frame_duration` instead of
> `next_frame_time = now + frame_duration`.
>
> However, if the handler is so slow that it misses an entire tick
> interval (meaning `mtime` is already past `mtimecmp + interval` by the
> time we update), the next interrupt fires immediately — which is the
> right behavior (catch up rather than skip).

**`msip[hart]`** (32-bit, read-write, per-hart):

Writing 1 to `msip[hart]` raises a **machine software interrupt** (MSI)
on that hart. Writing 0 clears it. This is used for inter-processor
interrupts (IPIs) — one hart can poke another to get its attention.

We won't use `msip` in this round (we only have one hart). It becomes
important in Phase 5 when we do multi-hart scheduling.

---

## Part 3: The Delegation Problem

### CSRs vs MMIO — three kinds of registers

Before diving into the timer problem, we need to clear up a potential
confusion. The CLINT registers (`mtime`, `mtimecmp`) have names that
start with `m`, just like M-mode CSRs (`mstatus`, `mie`, `mcause`).
But they are fundamentally different things. In fact, there are three
kinds of "registers" in a RISC-V system, each accessed differently:

| Category | Examples | Where they live | How to access |
|----------|---------|----------------|---------------|
| General-purpose registers | `zero`, `sp`, `a0`, `t0` | CPU core (register file) | Directly in instructions (`add a0, t0, t1`) |
| CSRs | `mstatus`, `sstatus`, `stvec` | CPU core (separate register file) | `csrr`/`csrw` with a 12-bit CSR number |
| MMIO registers | `mtime`, `mtimecmp`, UART THR | External hardware on the bus | `ld`/`sd` to physical addresses |

The first two are **internal to the CPU core** — accessing them never
involves the memory bus or a physical address. The third is **external
hardware** that happens to respond to memory reads and writes.

**General-purpose registers** (`x0`-`x31` / `zero`, `ra`, `sp`, `a0`,
etc.) are the 32 integer registers we've been using since Lecture 0-2.
They're referenced by register number directly in the instruction
encoding. No address, no bus, no privilege check.

**CSRs** (Control and Status Registers) are a separate set of up to
4096 registers inside the CPU. You access them with `csrr`/`csrw`, which
encode the CSR number as a 12-bit immediate in the instruction. CSR
`0x300` is `mstatus`, CSR `0x100` is `sstatus`, and so on. These
numbers are NOT memory addresses — they're indices into a CPU-internal
register file. You cannot do `ld t0, 0x300` to read `mstatus` — that
would issue a memory bus read to physical address `0x300` (which is
probably unmapped on QEMU `virt`, since RAM starts at `0x80000000`).

Each CSR has a hardcoded privilege level. The CPU checks this on every
`csrr`/`csrw`. The check is baked into the CSR number itself — bits
9:8 encode the minimum required privilege:

```
CSR number bits [11:10] [9:8]:

  Bits 9:8 = 0b00 (0x0xx) → accessible from U-mode and above
  Bits 9:8 = 0b01 (0x1xx) → accessible from S-mode and above
  Bits 9:8 = 0b10 (0x2xx) → reserved / hypervisor
  Bits 9:8 = 0b11 (0x3xx) → accessible from M-mode only

Examples:
  sstatus = 0x100 → bits 9:8 = 0b01 → S-mode ✓
  stvec   = 0x105 → bits 9:8 = 0b01 → S-mode ✓
  mstatus = 0x300 → bits 9:8 = 0b11 → M-mode only
  mie     = 0x304 → bits 9:8 = 0b11 → M-mode only
  time    = 0xC01 → bits 9:8 = 0b00 → U-mode ✓ (anyone can read)
```

When you execute `csrr t0, mstatus` from S-mode, the CPU checks:
bits 9:8 of `0x300` = `0b11` → M-mode required; current mode = S →
denied → illegal instruction exception (cause 2). This is the
privilege check that our Round 2-2 test trap exploits.

**MMIO** (Memory-Mapped I/O) registers live in external hardware
blocks connected to the system bus. You access them with regular
load/store instructions (`ld`, `sd`) to specific physical addresses.
The CPU doesn't know or care that `0x200BFF8` is a timer register —
it's just a memory address. The only access control is **PMP**
(Physical Memory Protection), and we configured PMP to allow S-mode
access to all physical memory.

```
CSR access (privilege-checked):        MMIO access (PMP-checked):

  csrr t0, mstatus                       ld t0, 0(a0)   # a0 = 0x200BFF8
       |                                      |
       v                                      v
  CPU decodes CSR number (0x300)         CPU issues memory load
  Bits 9:8 = 0b11 → M-mode required     Address goes out on the bus
  Current mode = S → DENIED                   |
  → exception 2 (illegal instruction)         v
                                         PMP check: S-mode allowed
                                         to access 0x200BFF8?
                                         Our PMP = allow all → YES
                                              |
                                              v
                                         CLINT hardware responds
                                         with the mtime value
```

So the `m` in `mtime` does NOT mean "M-mode access only." It means
"machine timer" — describing what the counter is *for*. Anyone with
physical memory access (which PMP grants us) can read it:

```c
// This works from S-mode! It's just a memory read.
uint64 now = *(volatile uint64 *)0x200BFF8;
```

Our UART driver already does exactly this — `uart_putc()` writes to
`0x10000000` from S-mode. The CLINT is the same story: MMIO registers
at known addresses, accessible from any mode that PMP permits.

| Name | Type | Access mechanism | S-mode accessible? |
|------|------|-----------------|:------------------:|
| `mstatus` | M-mode CSR (`0x300`) | `csrr`/`csrw` | No (bits 9:8 = `0b11`) |
| `mie` | M-mode CSR (`0x304`) | `csrr`/`csrw` | No (bits 9:8 = `0b11`) |
| `sstatus` | S-mode CSR (`0x100`) | `csrr`/`csrw` | Yes (bits 9:8 = `0b01`) |
| `time` | U-mode CSR (`0xC01`) | `csrr` (read-only) | Yes (bits 9:8 = `0b00`) |
| `mtime` | CLINT MMIO at `0x200BFF8` | `ld`/`sd` | Yes (if PMP allows) |
| `mtimecmp` | CLINT MMIO at `0x2004000` | `ld`/`sd` | Yes (if PMP allows) |
| UART THR | UART MMIO at `0x10000000` | `ld`/`sd` | Yes (if PMP allows) |

> **The `time` CSR — a read-only mirror of `mtime`**
>
> The base RISC-V spec defines a `time` CSR (address `0xC01`) that
> S-mode and U-mode can read. The spec says implementations *may*
> provide it as a read-only shadow of the memory-mapped `mtime`
> register. This would let S-mode read the time via `csrr t0, time`
> instead of MMIO. However, `time` is optional — many implementations
> (including QEMU's default) don't provide it, expecting the OS to
> read `mtime` via MMIO instead. We'll use the MMIO approach.

> **The boundary between "core" and "external" is blurry.** The CLINT
> is sometimes fabricated on the same silicon die as the CPU cores —
> it's "external" in the sense that it's on the memory bus, but
> physically it may be millimeters away on the same chip. The
> distinction is really about the **access mechanism** (bus transaction
> vs dedicated CSR instruction), not physical distance.
>
> And the trend is to move performance-critical registers *into* the
> core. ARM's generic timer (`CNTPCT_EL0`, `CNTP_CVAL_EL0`) is a
> system register (like a CSR), not MMIO, even though it's a timer
> just like the CLINT's `mtime`/`mtimecmp`. The RISC-V SSTC extension
> does the same: `stimecmp` is a CSR, replacing the MMIO `mtimecmp`.
> The more frequently a register is accessed, the more likely it moves
> from the bus into the core to avoid bus latency.

This distinction matters for understanding the core problem of this
section: S-mode *can* read and write the CLINT's MMIO registers, but
the interrupt the CLINT generates (MTIP) is a *machine-mode CSR
event* — and that's where the privilege wall blocks us.

### Machine timer interrupts can't be delegated

In Lecture 2-1, we delegated all interrupts and exceptions to S-mode:

```asm
li    t0, 0xffff
csrw  medeleg, t0    # delegate all exceptions
csrw  mideleg, t0    # delegate all interrupts
```

We wrote `0xffff` to `mideleg`, meaning "delegate everything." But the
RISC-V spec says certain interrupt bits in `mideleg` are **hardwired to
zero** — the hardware ignores our attempt to delegate them. Which ones?

| Interrupt | `mideleg` bit | Delegatable? |
|-----------|:------------:|:------------:|
| Supervisor software (SSIP, bit 1) | 1 | Yes |
| Machine software (MSIP, bit 3) | 3 | No (hardwired 0) |
| Supervisor timer (STIP, bit 5) | 5 | Yes |
| Machine timer (MTIP, bit 7) | 7 | **No (hardwired 0)** |
| Supervisor external (SEIP, bit 9) | 9 | Yes |
| Machine external (MEIP, bit 11) | 11 | No (hardwired 0) |

The pattern: **machine-level interrupts (bits 3, 7, 11) cannot be
delegated.** They always trap to M-mode, period. This is a security
property — M-mode firmware must always be able to handle its own
interrupts without the OS interfering.

The machine timer interrupt (bit 7, MTIP) is the one that matters for
us. The CLINT generates a *machine* timer interrupt — it sets the MTIP
bit in `mip`. We cannot tell the hardware to send it to S-mode instead.

Now, S-mode *can* read and write `mtimecmp` via MMIO (PMP allows it).
But that doesn't help — even if S-mode programs the deadline itself,
when `mtime >= mtimecmp`, the CLINT still asserts MTIP (a machine-mode
interrupt). The CLINT hardware has no output wire for STIP. There is
simply no path from `mtime >= mtimecmp` to a supervisor-mode interrupt
in the base RISC-V architecture.

### Three ways to deliver timer ticks to S-mode

This is a well-known problem, and the RISC-V ecosystem has three
solutions. Understanding all three clarifies why each interrupt bit
exists:

**Approach 1: Direct CLINT access + SSIP forwarding (what we do)**

Our kernel runs with `-bios none` — no firmware. Our own M-mode code
in `entry.S` programs the CLINT and handles timer interrupts directly.
The M-mode handler catches MTIP, updates `mtimecmp`, and raises SSIP
(supervisor software interrupt, bit 1) to signal S-mode. S-mode sees
`scause = 1` (software interrupt).

This is what xv6 does, and what bobchouOS will do.

**Approach 2: SBI firmware + STIP (the standard real-hardware path)**

Most real RISC-V systems run with **SBI firmware** (like OpenSBI) in
M-mode. The OS never touches the CLINT directly. Instead:

1. S-mode calls `sbi_set_timer(deadline)` via an `ecall` instruction
2. The `ecall` traps to M-mode (exception code 9, which we said in
   Lecture 2-2 is hardwired to not delegate)
3. M-mode firmware programs `mtimecmp` on the OS's behalf
4. When the timer fires, M-mode firmware sets `sip.STIP = 1`
5. S-mode sees `scause = 5` — a **supervisor timer interrupt**

In this world, the OS enables `sie.STIE` (bit 5) and handles scause=5
as the timer tick. **STIP is the *intended* path** — it's what the
supervisor timer interrupt bits were designed for. Our SSIP approach is
the workaround for when you don't have firmware.

Linux on real RISC-V hardware (pre-SSTC) uses this approach. If you
look at the Linux RISC-V timer driver, you'll find `sbi_set_timer()`
calls and a handler for `IRQ_S_TIMER` (cause 5), not `IRQ_S_SOFT`.

**Approach 3: SSTC extension + STIP (the modern path)**

Newer RISC-V chips implement the **SSTC extension** (Supervisor-level
Timer Compare), which adds two things:

- **`stimecmp`** — a new CSR that S-mode can write. It's a timer
  compare register, just like `mtimecmp`, but it triggers **STIP**
  instead of MTIP.
- **`time`** — the read-only CSR (address `0xC01`) that mirrors
  `mtime`, guaranteed to be implemented (not optional with SSTC).

When `time >= stimecmp`, the hardware directly sets STIP — no M-mode
involvement at all. S-mode programs its own deadline, reads the current
time, and receives the interrupt, all without a single trap to M-mode:

```
Without SSTC (our approach):
  CLINT: mtime >= mtimecmp  -->  MTIP (M-mode only)
                                   |
                                   v
                            M-mode handler forwards via SSIP
                                   |
                                   v
                            S-mode sees scause=1 (software)

With SSTC:
  CPU: time >= stimecmp  -->  STIP (S-mode directly!)
                                |
                                v
                          S-mode sees scause=5 (timer)
                          No M-mode bounce at all
```

This is the cleanest path: no SBI `ecall` overhead, no M-mode handler,
no SSIP forwarding. Modern Linux on RISC-V uses SSTC when available.

**Summary of the three approaches:**

| Approach | Who programs the timer | S-mode sees | Used by |
|----------|----------------------|------------|---------|
| Direct CLINT + SSIP | Our M-mode asm | scause=1 (software) | xv6, bobchouOS |
| SBI firmware + STIP | OpenSBI (M-mode) | scause=5 (timer) | Linux (pre-SSTC) |
| SSTC extension | S-mode directly | scause=5 (timer) | Linux (modern RISC-V) |

You might notice that both approaches 1 and 2 have a "middleman" in
M-mode — our `timer_vec` handler or OpenSBI firmware. If both need a
middleman, why is the SBI approach adopted by real hardware?

The answer isn't fewer layers — it's **portability**. Different chips
have different timer hardware. Some use the SiFive CLINT (like QEMU).
Some use the newer ACLINT (Advanced CLINT, which splits the timer and
software interrupt into separate devices at different addresses). Some
have proprietary timer blocks. Our `timer_vec` hardcodes the CLINT
register addresses — if the hardware changes, our code breaks. SBI
hides all of that behind a stable function call:

| | Our approach (direct CLINT) | SBI firmware |
|---|---|---|
| OS knows CLINT base address? | Yes (`0x2000000`) | No — firmware hides it |
| OS knows `mtimecmp` layout? | Yes (offset `0x4000 + 8*hart`) | No — firmware hides it |
| OS knows timer frequency? | Must hardcode or parse device tree | Can query SBI |
| Port to different chip? | Rewrite CLINT addresses | Just works (same SBI API) |
| Works without firmware? | Yes (`-bios none`) | No (needs OpenSBI or similar) |

It's the same reason you call `printf()` instead of writing bytes to a
display controller — abstraction lets you swap the implementation
without changing the caller. The chip vendor ships OpenSBI with their
board, programmed to talk to whatever timer hardware they designed. The
OS calls `sbi_set_timer(deadline)` and doesn't care about the details.

But both approaches still pay a **performance cost** — every timer tick
bounces through M-mode. And the SBI approach adds an additional
round-trip for the `sbi_set_timer()` ecall:

| Approach | Mode transitions per tick | Portability |
|----------|:------------------------:|:-----------:|
| Direct CLINT + SSIP (us) | 2 (S→M→S) | Hardware-specific |
| SBI + STIP | 2 + extra ecall to set next deadline | Portable |
| SSTC | 0 (pure S-mode) | Portable AND fast |

This is why the SSTC extension was created. SBI was the pragmatic
solution for early RISC-V chips — it works everywhere, the cost is
acceptable. But SSTC is the long-term fix: **remove the middleman
entirely**, not just move it into firmware. With SSTC, S-mode writes
`stimecmp` directly and gets STIP directly. No traps, no firmware, no
overhead. Portable (it's a CSR, same on every chip that implements
SSTC) and fast.

So **STIP (bit 5) is the "proper" design** — it's what the RISC-V
architects intended for S-mode timer interrupts. **SSIP (bit 1) is the
workaround** — it's designed for inter-hart messaging (one hart poking
another), but we repurpose it as a timer signal because of our
`-bios none` setup. Both approaches work; STIP is more semantically
correct, SSIP is simpler for teaching.

### The forwarding trick (our approach in detail)

Since we run with `-bios none` and don't have SSTC, we use the SSIP
forwarding trick. Here's the relay chain:

```
CLINT hardware                M-mode handler            S-mode kernel
--------------                ---------------           ---------------
mtime >= mtimecmp
   |
   +-> MTIP fires
       (machine timer         timer_vec catches it
        interrupt)            |
                              +-> update mtimecmp
                              |   (schedule next tick)
                              |
                              +-> set sip.SSIP = 1
                              |   (raise supervisor
                              |    software interrupt)
                              |
                              +-> mret
                                                        SSIP fires
                                                        (supervisor software
                                                         interrupt, cause 1)
                                                        |
                                                        +-> kernel_trap()
                                                        |   handles "timer tick"
                                                        |   clears sip.SSIP
                                                        |   increments counter
                                                        +-> sret (resume)
```

The M-mode handler acts as a translator:
- **Input:** machine timer interrupt (MTIP, bit 7) — can only be
  received in M-mode
- **Output:** supervisor software interrupt (SSIP, bit 1) — can be
  received in S-mode

Why SSIP specifically? Because M-mode can write to `sip` (the
supervisor interrupt pending register). Setting bit 1 of `sip` is like
ringing S-mode's doorbell — it says "hey, you have a supervisor
software interrupt pending." When S-mode has `sie.SSIE` enabled
(supervisor software interrupt enable, bit 1) and `sstatus.SIE` = 1
(global interrupt enable), the CPU will trap into S-mode's trap handler.

> **Why SSIP instead of STIP for forwarding?**
>
> You might wonder: if M-mode firmware (approach 2) sets STIP, why
> doesn't xv6 also set STIP instead of SSIP?
>
> The reason is about who controls the bit. Not all `sip` bits are
> writable from S-mode — this is **enforced by hardware**, not a
> convention:
>
> | `sip` bit | Name | S-mode writable? | Controlled by |
> |:---------:|------|:----------------:|---------------|
> | 1 | SSIP | Yes | Software (any mode) |
> | 5 | STIP | **No (read-only)** | M-mode firmware or SSTC hardware |
> | 9 | SEIP | **No (read-only)** | PLIC (external interrupt controller) |
>
> When S-mode executes `csrw sip, val`, the hardware silently ignores
> writes to bits 5 and 9 — the same "hardwired" masking we saw with
> `mideleg`. No exception is raised; the write just has no effect on
> those bits. STIP and SEIP are read-only from S-mode because they're
> controlled by external sources: if S-mode could clear STIP, it could
> suppress timer interrupts that M-mode firmware is trying to deliver.
>
> This means if we used STIP for forwarding, S-mode couldn't clear the
> pending bit after handling the tick, and we'd need another `ecall` to
> M-mode just to clear it. With SSIP — the only S-mode writable pending
> bit — S-mode handles everything itself.
>
> SBI firmware (approach 2) can use STIP because the firmware clears it
> on the next `sbi_set_timer()` call — the clearing is part of the SBI
> contract. Without SBI, SSIP is simpler.

### The full interrupt enable chain

For a timer tick to reach `kernel_trap()`, several enable bits must all
be set. Think of it as a chain of gates — every gate must be open:

```
CLINT                M-mode enables           S-mode enables
-----                ---------------          ---------------
mtime >= mtimecmp    mie.MTIE = 1?            sie.SSIE = 1?
  |                     |                        |
  v                     v                        v
MTIP pending ------> M-mode trap? ---------> S-mode trap?
                     (mstatus.MIE=1 or       (sstatus.SIE=1)
                      lower priv mode)
                           |                        |
                           v                        v
                     timer_vec runs          kernel_trap runs
                     sets SSIP=1            handles "timer tick"
```

Let's trace each gate:

**Gate 1: `mie.MTIE` (Machine Interrupt Enable, Timer bit)**

The `mie` register has per-interrupt-type enable bits. Bit 7 is MTIE
(Machine Timer Interrupt Enable). If MTIE = 0, the machine timer
interrupt is masked — `mtime >= mtimecmp` sets the pending bit but the
CPU doesn't trap.

We need to set `mie.MTIE = 1` in our M-mode startup code.

**Gate 2: `mstatus.MIE` (Machine global interrupt enable)**

Even if `mie.MTIE` is set, the CPU only takes M-mode interrupts when
`mstatus.MIE` = 1 — *unless* the CPU is running at a lower privilege
level. The RISC-V spec says:

> "Interrupts for higher-privilege modes ... are always globally
> enabled regardless of the setting of the global ... bit for the
> higher-privilege mode."

In other words: when running in S-mode, M-mode interrupts are *always*
taken, regardless of `mstatus.MIE`.

The `mstatus.MIE` bit only matters when you're already in M-mode — it
prevents nesting (the hardware clears it on M-mode trap entry, just
like `sstatus.SIE` on S-mode trap entry). Since our kernel runs in
S-mode after `mret`, the timer interrupt will fire regardless of what
`mstatus.MIE` is set to.

**Gate 3: `sie.SSIE` (Supervisor Interrupt Enable, Software bit)**

The `sie` register has per-interrupt-type enable bits for S-mode.
Bit 1 is SSIE (Supervisor Software Interrupt Enable). When the M-mode
`timer_vec` sets `sip.SSIP = 1`, S-mode will only trap if `sie.SSIE = 1`.

We need to set `sie.SSIE = 1` in `kmain()`.

**Gate 4: `sstatus.SIE` (Supervisor global interrupt enable)**

The master switch for all S-mode interrupts. Even if `sie.SSIE` is
set and SSIP is pending, the CPU won't trap to S-mode unless
`sstatus.SIE = 1`.

Recall from Lecture 2-2 that the hardware clears `sstatus.SIE` on trap
entry (to prevent nested interrupts) and `sret` restores it from SPIE.
So the flow is:

1. `sstatus.SIE = 1` — interrupts enabled, kernel running normally
2. Timer fires → SSIP set → S-mode trap → hardware clears SIE to 0
3. Handler runs with interrupts disabled
4. Handler does `sret` → hardware restores SIE from SPIE → SIE = 1
5. Interrupts enabled again

We need to set `sstatus.SIE = 1` in `kmain()` after setting up
everything.

### The enable bits at a glance

```
M-mode enables (set in entry.S, before mret):
+---------------------------------------------------+
|  mie register:                                    |
|    bit 7 (MTIE) = 1   Machine Timer Int Enable    |
|                                                   |
|  mstatus register:                                |
|    MIE doesn't matter once we're in S-mode        |
|    (M-mode interrupts always enabled from S)      |
+---------------------------------------------------+

S-mode enables (set in kmain, after trap setup):
+---------------------------------------------------+
|  sie register:                                    |
|    bit 1 (SSIE) = 1   Supervisor Software Enable  |
|                                                   |
|  sstatus register:                                |
|    bit 1 (SIE) = 1    Global interrupt enable     |
+---------------------------------------------------+
```

---

## Part 4: The M-mode Timer Handler (`timer_vec`)

### The challenge

When the machine timer interrupt fires, the CPU traps to whatever
address is in `mtvec`. This handler runs in M-mode — the most
privileged level. It must:

1. Save any registers it uses (without corrupting anything)
2. Schedule the next timer tick (`mtimecmp += interval`)
3. Raise SSIP to notify S-mode
4. Restore registers
5. `mret` back to S-mode

The tricky part is step 1: **how do you save registers when you have no
free registers?** Saving a register requires `sd a0, offset(base)` —
but the base register must already hold the save-area address. Loading
that address (e.g., `la t0, save_area`) would destroy `t0`'s value
before we saved it. Every register belongs to whatever S-mode code was
running. Clobbering any of them corrupts the kernel.

### The `mscratch` trick

This is the same problem we discussed in Lecture 2-2 with `sscratch`,
but now at M-mode with `mscratch`. The solution:

Before entering S-mode (during boot), we set `mscratch` to point to a
small **scratch area** in memory. This area has room for:

- 3 register save slots (to temporarily store registers we'll use)
- The address of `mtimecmp[hart]` (pre-computed)
- The timer interval value (pre-computed)

```
Scratch area layout (5 x 8 bytes = 40 bytes):

Offset  Contents                Purpose
------  ----------------------  --------------------------------
  0     (save slot)             temp storage for a register
  8     (save slot)             temp storage for a register
 16     (save slot)             temp storage for a register
 24     &mtimecmp[hartid]       pre-computed CLINT address
 32     interval                timer period in mtime ticks
```

The handler starts with:

```asm
csrrw  a0, mscratch, a0     # swap a0 with mscratch
```

After this single instruction:
- `a0` = old `mscratch` = pointer to our scratch area
- `mscratch` = old `a0` = the interrupted code's a0 (safe for now)

Now the handler can save more registers into the scratch area:

```asm
sd     a1, 0(a0)            # save a1 to scratch[0]
sd     a2, 8(a0)            # save a2 to scratch[1]
sd     a3, 16(a0)           # save a3 to scratch[2]
```

Now we have 3 free registers (a1, a2, a3) to work with. That's enough
to update `mtimecmp` and set SSIP.

### Why pre-compute values in the scratch area?

The handler needs to know:
- Where `mtimecmp[0]` is in memory (address `0x2004000`)
- What interval to add (e.g., 100,000 for 10ms at 10 MHz)

We *could* hardcode these as immediate values in the assembly. But
loading a 64-bit address into a register takes multiple instructions
(`lui` + `addi` or `la` pseudo-instruction). By storing pre-computed
values in the scratch area, the handler just does `ld a1, 24(a0)` —
one instruction. This keeps the handler tiny and fast.

xv6 does the same thing: `timerinit()` (in C, running in M-mode during
boot) fills the scratch area with the right values, then the assembly
handler just loads them.

### The complete handler

Here's the full `timer_vec` handler:

```asm
.align 4
timer_vec:
    # a0 now points to the scratch area (swapped with mscratch)
    csrrw a0, mscratch, a0

    # Save the registers we'll use
    sd    a1, 0(a0)          # scratch[0] = a1
    sd    a2, 8(a0)          # scratch[1] = a2
    sd    a3, 16(a0)         # scratch[2] = a3

    # Schedule the next timer interrupt:
    # mtimecmp[hart] = mtimecmp[hart] + interval
    ld    a1, 24(a0)         # a1 = &mtimecmp[hart]
    ld    a2, 0(a1)          # a2 = current mtimecmp value
    ld    a3, 32(a0)         # a3 = interval
    add   a2, a2, a3         # a2 = mtimecmp + interval
    sd    a2, 0(a1)          # write new mtimecmp

    # Raise a supervisor software interrupt (SSIP, sip bit 1)
    # so S-mode's kernel_trap() will handle the tick
    li    a1, 2              # bit 1
    csrs  sip, a1            # sip |= 2  (set SSIP)

    # Restore saved registers
    ld    a1, 0(a0)
    ld    a2, 8(a0)
    ld    a3, 16(a0)

    # Restore a0 from mscratch and put scratch pointer back
    csrrw a0, mscratch, a0

    mret
```

This is remarkably small — about 15 instructions. No C code, no stack,
no complex logic. It does exactly two things: schedule the next tick
and ring S-mode's doorbell. All the real work (counting ticks, deciding
whether to preempt) happens in S-mode's `kernel_trap()`.

> **Can M-mode access `sip`?** Yes. M-mode can read and write all CSRs,
> including S-mode ones. This is the whole point of the privilege
> hierarchy — higher modes can access lower modes' state. S-mode cannot
> access M-mode CSRs (like `mie`, `mstatus`, `mscratch`), but M-mode
> can freely touch `sip`, `sie`, `sstatus`, etc.

> **What about the `csrs` instruction?** `csrs sip, a1` is shorthand
> for `csrrs zero, sip, a1` — it atomically sets the bits in `sip`
> that are 1 in `a1`, without affecting other bits. The complementary
> `csrc sip, a1` clears bits. These are the same `csrrs`/`csrrc`
> instructions we saw in Lecture 2-2, just with a shorter alias.
>
> ```
> csrs  csr, rs    <==>    csrrs  zero, csr, rs     # set bits
> csrc  csr, rs    <==>    csrrc  zero, csr, rs     # clear bits
> ```

### Why no stack?

Notice that `timer_vec` never touches `sp`. It doesn't push anything
onto a stack. Instead, it uses the scratch area for all its temporary
storage. Why?

1. **We don't know which stack is active.** When the timer fires, the
   CPU could be running any S-mode code. The current `sp` points to
   S-mode's kernel stack. Using it from M-mode would work (M-mode can
   access all memory), but it's mixing M-mode and S-mode concerns.

2. **Minimalism.** The handler uses only 3 registers (a1-a3), which
   fit in the scratch area. No need for a stack frame.

3. **Speed.** No stack pointer manipulation = fewer instructions =
   faster handler. Timer interrupts should be as fast as possible
   because they fire frequently and delay whatever S-mode was doing.

xv6's `timervec` uses the exact same approach: scratch area, no stack.

---

## Part 5: Setting Up the Timer (M-mode Initialization)

### What we need to do before `mret`

Our M-mode boot code in `entry.S` currently does 9 steps (park harts,
zero BSS, set stack, configure MPP, set mepc, disable paging, delegate
traps, set PMP, mret). We need to add timer setup between the PMP
configuration and `mret`.

The timer setup involves:

1. **Allocate a scratch area** for `timer_vec` (5 x 8 = 40 bytes)
2. **Fill the scratch area** with `&mtimecmp[0]` and the interval
3. **Set `mscratch`** to point to the scratch area
4. **Program the first deadline** (`mtimecmp[0] = mtime + interval`)
5. **Enable machine timer interrupt** (`mie.MTIE`, bit 7)
6. **Set `mtvec`** to point to `timer_vec`

After `mret`, the CPU enters S-mode. The timer is already ticking.
When `mtime` reaches the first deadline, the CPU traps to M-mode
(`timer_vec`), which updates the deadline and pokes S-mode via SSIP.

### The scratch area — where does it live?

We need 40 bytes of memory that persists across the boot and stays
valid while the kernel runs. The simplest approach: a global array
in BSS.

```c
// In a C file, or as a .bss allocation in assembly:
__attribute__((aligned(16)))
uint64 timer_scratch[5];
```

Or in assembly:

```asm
.section .bss
.align 4
timer_scratch:
    .zero 40          # 5 x 8 bytes
```

Since `entry.S` runs before any C code and we're doing everything in
assembly, we'll define `timer_scratch` in the `.bss` section. BSS is
zeroed by step 2 of our boot sequence, so all slots start at 0.

> **Why not define it in the linker script, like the stack?** Either
> would work, but there's a convention:
>
> | What | Define in | Why |
> |------|----------|-----|
> | Memory regions / layout | Linker script | Anonymous reservations — "give me 16KB and a top address" |
> | Named variables used by code | Source files (`.S` or `.c`) | Keeps data close to the code that uses it |
>
> The stack is anonymous memory — nothing references `stack` as a
> variable, we just need the `stack_top` symbol to load into `sp` once.
> The linker script is ideal for that (`. += 0x4000; stack_top = .;`).
> The scratch area is different: `timer_vec` actively loads from and
> stores to `timer_scratch` by name. Defining it in the same source
> file keeps the data and code together, easier to read and maintain.

### Programming the first deadline

To set the first timer deadline, we need to:

1. Read `mtime` (current time)
2. Add the interval
3. Write the result to `mtimecmp[0]`

The interval determines how often timer interrupts fire. xv6 uses
1,000,000 ticks (100ms at 10 MHz). We'll use a shorter interval:
**100,000 ticks = 10ms = 100 Hz**, which matches Linux's server
default. We'll print a message every 100 ticks (once per second)
rather than on every tick, keeping the console readable.

```
interval = 100,000    (decimal)
         = 0x186A0    (hex)

At 10 MHz:
  100,000 / 10,000,000 = 0.01 seconds = 10ms
  100 ticks/second
```

> **How many instructions fit in a tick?** Since timer ticks drive
> process scheduling, the interval directly affects how responsive the
> system feels. QEMU doesn't have a fixed CPU frequency (it's an
> emulator — instructions run at whatever speed the host allows), so
> let's use real hardware as a reference. A typical RISC-V CPU at 1 GHz
> (like the SiFive U74 in the HiFive Unmatched) executes roughly 1
> instruction per cycle on average (in-order pipeline):
>
> | Interval | Inst. per tick (@ 1 GHz) | Who uses it |
> |----------|:------------------------:|-------------|
> | 1ms (1000 Hz) | 1,000,000 | Linux (low-latency / audio) |
> | 4ms (250 Hz) | 4,000,000 | Linux (desktop default) |
> | **10ms (100 Hz)** | **10,000,000** | **Linux (server), bobchouOS** |
> | 100ms (10 Hz) | 100,000,000 | xv6 |
>
> At 10ms, each process gets 10 million instructions per time slice —
> plenty of work per tick, and context switches feel instant to humans
> (we notice delays above 30-50ms). xv6 uses 100ms for simplicity, but
> that would feel sluggish in an interactive system.
>
> The tradeoff is overhead vs responsiveness. Each tick has two trap
> round-trips (M-mode and S-mode), totaling about 100-150 instructions
> of handler overhead (register saves/restores, handler logic, hardware
> pipeline flushes). At 1 GHz, that's roughly 100-150 nanoseconds —
> about 0.002% of a 10ms tick. Completely negligible. You'd need
> sub-microsecond ticks before the overhead becomes a real problem.

### Filling the scratch area

In `entry.S`, after the scratch area is defined, we fill it:

```asm
# Load the scratch area address
la    t0, timer_scratch

# scratch[3] = &mtimecmp[0]
li    t1, 0x2004000        # CLINT + 0x4000 for hart 0
sd    t1, 24(t0)           # scratch[3] = &mtimecmp[0]

# scratch[4] = interval (100,000 = 10ms at 10 MHz)
li    t1, 100000
sd    t1, 32(t0)           # scratch[4] = interval

# Set mscratch = &timer_scratch
csrw  mscratch, t0

# Program first deadline: mtimecmp[0] = mtime + interval
li    t1, 0x200BFF8        # &mtime
ld    t1, 0(t1)            # t1 = current mtime
li    t2, 100000           # interval
add   t1, t1, t2           # t1 = mtime + interval
li    t2, 0x2004000        # &mtimecmp[0]
sd    t1, 0(t2)            # set first deadline
```

### Setting `mtvec` and enabling MTIE

```asm
# Point mtvec to our M-mode timer handler (direct mode).
# mtvec has the same layout as stvec (bits 1:0 = MODE, bits 63:2 = BASE).
# timer_vec is .align 4 (16-byte aligned), so bits 1:0 = 00 = direct mode.
la    t0, timer_vec
csrw  mtvec, t0

# Enable machine timer interrupt (mie bit 7)
li    t0, (1 << 7)         # MTIE
csrs  mie, t0              # mie |= 0x80
```

> **Why only enable MTIE (bit 7) and not MSIE (bit 3) or MEIE (bit 11)?**
>
> The other two machine-mode interrupts serve purposes we don't need:
>
> - **Bit 3 (MSIE, machine software interrupt)** — used for
>   inter-processor interrupts (IPIs). One hart writes to another
>   hart's `msip` CLINT register to say "hey, reschedule." We only run
>   hart 0, so nobody is sending us IPIs. This becomes relevant in
>   Phase 5+ if we add multi-hart scheduling.
> - **Bit 11 (MEIE, machine external interrupt)** — used for device
>   interrupts (UART, disk) routed through the PLIC to M-mode. We
>   don't want device interrupts in M-mode — when we set up the PLIC
>   later, we'll configure it to target S-mode directly (using
>   `sie.SEIE`, bit 9), bypassing M-mode entirely.
>
> **Is it safe that `mtvec` points to `timer_vec` for all M-mode traps?**
> In Lecture 2-1, we didn't set `mtvec` because all traps were delegated
> to S-mode. Now `mtvec = timer_vec`, so any M-mode trap will jump there.
> But since we only enable MTIE, the timer is the only M-mode interrupt
> that can actually fire. Everything else goes to S-mode via delegation.
> If something unexpected did trap to M-mode, `timer_vec` would
> mishandle it — but that shouldn't happen in our controlled QEMU
> environment.

---

## Part 6: The S-mode Side — Handling Timer Ticks

### Enabling SSIE and SIE

In `kmain()`, after setting `stvec`, we need to enable:

1. **`sie.SSIE`** (bit 1) — permit supervisor software interrupts
2. **`sstatus.SIE`** (bit 1) — enable S-mode interrupts globally

```c
// Enable supervisor software interrupt (for timer ticks)
uint64 sie_val = csrr(sie);
sie_val |= (1 << 1);    // SSIE = bit 1
csrw(sie, sie_val);

// Enable interrupts globally
uint64 sstatus_val = csrr(sstatus);
sstatus_val |= (1 << 1);  // SIE = bit 1
csrw(sstatus, sstatus_val);
```

Or more concisely with the bit-set CSR operations:

```c
csrw(sie, csrr(sie) | SIE_SSIE);
csrw(sstatus, csrr(sstatus) | SSTATUS_SIE);
```

### Updating `kernel_trap()` to handle timer ticks

Currently, `kernel_trap()` panics on any interrupt. We need to recognize
supervisor software interrupt (scause = `SCAUSE_INTERRUPT | IRQ_S_SOFT`,
i.e., `0x8000000000000001`) and handle it as a timer tick:

```c
// Global tick counter
volatile uint64 ticks = 0;

void
kernel_trap(void)
{
    uint64 sepc_val    = csrr(sepc);
    uint64 scause_val  = csrr(scause);
    uint64 stval_val   = csrr(stval);
    uint64 sstatus_val = csrr(sstatus);

    // Sanity checks (same as before)
    if (!(sstatus_val & SSTATUS_SPP))
        panic("kernel_trap: not from S-mode");
    if (sstatus_val & SSTATUS_SIE)
        panic("kernel_trap: interrupts enabled during trap");

    if (scause_val & SCAUSE_INTERRUPT) {
        uint64 code = scause_val & 0xff;
        if (code == IRQ_S_SOFT) {
            // Timer tick (forwarded from M-mode via SSIP).
            // Clear the SSIP bit so we don't re-trap immediately.
            csrw(sip, csrr(sip) & ~(1UL << 1));
            ticks++;
            if (ticks % 100 == 0)
                kprintf("timer: %d seconds\n", (int)(ticks / 100));
        } else {
            panic("kernel_trap: unexpected interrupt code=%d", code);
        }
    } else {
        panic("kernel_trap: exception scause=%p sepc=%p stval=%p",
              scause_val, sepc_val, stval_val);
    }
}
```

### Why clear SSIP manually?

When M-mode's `timer_vec` does `csrs sip, 2` (set SSIP), that bit
stays set until someone clears it. The hardware does NOT clear it
automatically on S-mode trap entry — unlike how MTIP is cleared by
writing `mtimecmp`.

If we don't clear SSIP, the handler returns via `sret`, `sstatus.SIE`
gets restored to 1, the CPU sees SSIP still pending, and immediately
traps again. Infinite loop.

So the handler must clear SSIP: `csrw(sip, csrr(sip) & ~2)` or
equivalently `csrc sip, 2` in assembly.

> **Why the `volatile` on `ticks`?** The `ticks` variable is modified
> inside the trap handler (interrupt context) and read from `kmain()`
> (normal context). Without `volatile`, the compiler might optimize
> away the read in `kmain()`'s loop, deciding that `ticks` never
> changes from its perspective. `volatile` tells the compiler "this
> variable can change at any time — always read it from memory, never
> cache it in a register."
>
> In a real OS with multiple harts, you'd also need a lock or atomic
> operations. But with one hart and interrupts disabled during the
> handler, `volatile` is sufficient.

### Removing the test trap

The test trap from Round 2-2 (`csrr(mhartid)` in `kmain()`) must go.
It causes the kernel to halt, and we want the kernel to keep running so
it can receive timer interrupts. We'll replace it with a spin loop:

```c
void
kmain(void)
{
    uart_init();
    csrw(stvec, (uint64)kernel_vec);

    // ... enable sie.SSIE and sstatus.SIE ...

    kprintf("bobchouOS is booting...\n");
    kprintf("timer interrupts enabled, waiting for ticks...\n");

    // Spin forever — timer interrupts will fire periodically
    for (;;)
        ;
}
```

Every 10ms, the timer fires, `timer_vec` catches it in M-mode, sets
SSIP, S-mode traps to `kernel_trap()`, which increments `ticks` and
prints a message every 100 ticks (once per second). The kernel is alive
and responsive.

---

## Part 7: How xv6 Does It

### `timerinit()` — xv6's timer setup (M-mode C code)

xv6 runs its M-mode initialization in C (in `start.c`) before dropping
to S-mode. The `timerinit()` function does exactly what we described in
Part 5:

```c
// kernel/start.c — xv6's M-mode timer initialization (simplified)

void
timerinit()
{
    // Each hart gets its own scratch area.
    int id = r_mhartid();

    // Ask the CLINT for a timer interrupt after 'interval' ticks.
    int interval = 1000000;   // 100ms at 10 MHz
    *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + interval;

    // Prepare the scratch area for timervec.
    // scratch[0..2] = space for timervec to save registers
    // scratch[3]    = address of CLINT_MTIMECMP for this hart
    // scratch[4]    = desired interval
    uint64 *scratch = &timer_scratch[id][0];
    scratch[3] = CLINT_MTIMECMP(id);
    scratch[4] = interval;
    w_mscratch((uint64)scratch);

    // Set the machine-mode trap handler to timervec.
    w_mtvec((uint64)timervec);

    // Enable machine-mode interrupts (for timer only).
    w_mstatus(r_mstatus() | MSTATUS_MIE);

    // Enable machine-mode timer interrupt.
    w_mie(r_mie() | MIE_MTIE);
}
```

Note that xv6 calls this from `start()` which runs in M-mode, before
`mret` to S-mode. Each hart calls `timerinit()` independently (they
each need their own `mscratch` and `mtimecmp`).

> **xv6 sets `MSTATUS_MIE`?** Yes — xv6 enables `mstatus.MIE` in
> `timerinit()`. This seems surprising because we said M-mode
> interrupts are always taken from S-mode regardless of MIE. The
> reason is that xv6 calls `timerinit()` while still in M-mode (before
> `mret`). Setting MIE means the first timer interrupt can fire even
> before the mode switch completes. This is fine because `mtvec` is
> already set to `timervec`.
>
> In bobchouOS, we do the setup in assembly and `mret` immediately.
> We could enable MIE too, but it doesn't matter much — the first
> timer deadline is `mtime + interval` which is 10ms in the future,
> long after our `mret` executes (which takes nanoseconds). Either way
> works.

### `timervec` — xv6's M-mode timer vector

xv6's `timervec` in `kernel/kernelvec.S` is almost identical to what
we described in Part 4:

```asm
# kernel/kernelvec.S — xv6's machine-mode timer vector (simplified)

.globl timervec
.align 4
timervec:
        # scratch[0,8,16] : register save area
        # scratch[24]     : address of CLINT_MTIMECMP
        # scratch[32]     : desired interval (in cycles)
        csrrw a0, mscratch, a0
        sd a1, 0(a0)
        sd a2, 8(a0)
        sd a3, 16(a0)

        # schedule the next timer interrupt
        # by adding interval to mtimecmp.
        ld a1, 24(a0)    # CLINT_MTIMECMP address
        ld a2, 0(a1)     # current mtimecmp
        ld a3, 32(a0)    # interval
        add a2, a2, a3
        sd a2, 0(a1)     # write new mtimecmp

        # arrange for a supervisor software interrupt
        # after this handler returns.
        li a1, 2
        csrw sip, a1

        ld a1, 0(a0)
        ld a2, 8(a0)
        ld a3, 16(a0)
        csrrw a0, mscratch, a0

        mret
```

One minor difference: xv6 does `csrw sip, a1` (write 2 to `sip`,
replacing the entire value) rather than `csrs sip, a1` (set bit 1).
The effect is the same because SSIP (bit 1) is the only bit we care
about, and the `sip` register from S-mode's view only has a few
writable bits anyway.

### `devintr()` and `kerneltrap()` — S-mode handling

In xv6, the S-mode side is handled by `devintr()` (which identifies
the interrupt source) and `kerneltrap()` (which acts on it):

```c
// kernel/trap.c — xv6's device interrupt identification (simplified)

int
devintr()
{
    uint64 scause = r_scause();

    if ((scause & 0x8000000000000000L) &&
        (scause & 0xff) == 9) {
        // Supervisor external interrupt (PLIC)
        // ... claim from PLIC, handle device ...
        return 1;
    } else if (scause == 0x8000000000000001L) {
        // Supervisor software interrupt (timer tick forwarded from M-mode)
        if (cpuid() == 0) {
            clockintr();    // only one hart updates the tick counter
        }
        w_sip(r_sip() & ~2);  // clear SSIP
        return 2;             // 2 = timer interrupt
    } else {
        return 0;             // unrecognized
    }
}
```

And in `kerneltrap()`:

```c
void
kerneltrap()
{
    // ... save sepc, sstatus ...

    if (scause & (1L << 63)) {
        int which_dev = devintr();
        if (which_dev == 2 && myproc() != 0
            && myproc()->state == RUNNING)
            yield();     // timer → preempt current process
    } else {
        panic("kerneltrap");  // exception in kernel = fatal
    }

    // ... restore sepc, sstatus ...
}
```

The `yield()` call is what makes timer interrupts useful for
scheduling — it gives other processes a chance to run. We'll add
that in Phase 5. For now, our handler just counts ticks.

---

## Part 8: The `mie` and `sie` Registers — A Closer Look

### Register layout

The `mie`/`sie` (interrupt enable) registers use the same bit positions
as the `mip`/`sip` (interrupt pending) registers — bit 1 is always
software, bit 5 is always timer, bit 9 is always external. The "enable"
registers gate whether a pending interrupt can actually fire. Here's the
layout for the enable side:

```
Bit    Enable (mie/sie)              Pending (mip/sip)
----   ---------------------------   ---------------------------
       mie              sie          mip              sip
       ---------------  ----------   ---------------  ----------
 1     S software IE    S soft IE    S software IP    S soft IP
 3     M software IE    (hidden)     M software IP    (hidden)
 5     S timer IE       S timer IE   S timer IP       S timer IP
 7     M timer IE       (hidden)     M timer IP       (hidden)
 9     S external IE    S ext IE     S external IP    S ext IP
11     M external IE    (hidden)     M external IP    (hidden)
```

**`mie`** is the M-mode view — it can see and control all interrupt
enable bits, both machine-level and supervisor-level.

**`sie`** is the S-mode view — it can only see and control the
supervisor-level bits (1, 5, 9). The machine-level bits (3, 7, 11) are
not accessible from S-mode. (Technically, `sie` is a "restricted view"
of `mie` — reading `sie` returns only the supervisor bits of `mie`.)

For Round 2-3, we need:
- `mie.MTIE` (bit 7) = 1 — set in M-mode before `mret`
- `sie.SSIE` (bit 1) = 1 — set in S-mode (`kmain`)

We do NOT set `sie.STIE` (bit 5) because we're not using the
supervisor timer interrupt path — we're using the software interrupt
forwarding trick instead.

### `sip` vs `scause` — "what's waiting" vs "what just happened"

The pending bits in `sip` tell you which interrupt types are currently
waiting. You might wonder: if `sip` already identifies the interrupt,
why do we need a separate `scause` register?

Two reasons:

**1. `scause` covers exceptions, not just interrupts.** `sip` only has
bits for interrupts (software, timer, external). When a page fault or
illegal instruction fires, no `sip` bit is set — `scause` is the only
way to know what happened.

**2. Multiple `sip` bits can be set simultaneously.** Suppose bits 1,
5, and 9 are all pending at once — three different interrupts waiting.
The CPU only traps for *one* of them. Which one? `sip` doesn't tell
you — it shows the full pending state. `scause` tells you exactly
which interrupt caused *this particular* trap:

```
sip = 0b...0000_0010_0010   (bits 1 and 5 both set)
                              SSIP and STIP both pending

scause = 0x8000000000000001  "you trapped because of bit 1 (SSIP)"
                              (bit 5 is still pending, will fire next)
```

So `sip` is "what's waiting" and `scause` is "what just happened."
After handling the current interrupt and doing `sret`, the CPU checks
`sip` again and immediately traps for the next pending one.

### Hardware interrupt priority

When multiple interrupts are pending, who goes first? The **hardware**
decides, using a fixed priority order defined by the RISC-V spec.
Machine-level beats supervisor-level, and within each level, external
beats software beats timer:

| Priority | Interrupt | Bit |
|:--------:|-----------|:---:|
| Highest | Machine external (MEI) | 11 |
| | Machine software (MSI) | 3 |
| | Machine timer (MTI) | 7 |
| | Supervisor external (SEI) | 9 |
| | Supervisor software (SSI) | 1 |
| Lowest | Supervisor timer (STI) | 5 |

For bobchouOS right now, this barely matters. The only S-mode interrupt
we enable is SSIP (bit 1). We don't enable STIE (bit 5) or SEIE
(bit 9). So even if multiple `sip` bits are set, only SSIP can actually
cause a trap — the others are pending but gated by `sie`.

When we add PLIC support later (enabling SEIE for UART interrupts),
both SSIP and SEIP could be pending simultaneously. The hardware picks
SEIP first (higher priority), we handle it, `sret`, then SSIP fires
immediately. This is all transparent to our code — `kernel_trap()` just
reads `scause` and handles whatever it says. We don't need to implement
priority logic ourselves.

### The complete interrupt enable hierarchy

```
Hardware                 M-mode                   S-mode
--------                 ------                   ------

CLINT timer fires
  |
  v
mip.MTIP = 1 (pending)
  |
  +--- mie.MTIE = 1? ---> mstatus.MIE = 1     (or lower mode)
  |         |                    |
  |         no? masked.          v
  |                         M-mode trap
  |                         timer_vec runs
  |                           |
  |                           v
  |                      sip.SSIP = 1 (M-mode sets it)
  |                           |
  +                           +--- sie.SSIE = 1? ---> sstatus.SIE = 1?
                              |         |                    |
                              |         no? masked.          v
                              |                         S-mode trap
                              |                         kernel_trap runs
                              |                         clears SSIP
                              |                         counts tick
```

---

## Part 9: How bobchouOS Will Do It

### The plan

We need changes in three places:

**1. M-mode timer setup (in `entry.S`, before `mret`):**
- Allocate and fill a scratch area for `timer_vec`
- Program the first timer deadline
- Set `mtvec` to `timer_vec`
- Enable `mie.MTIE`

**2. M-mode timer handler (new code, in `entry.S` or new file):**
- `timer_vec`: the tiny assembly handler that schedules the next tick
  and raises SSIP

**3. S-mode changes (in `main.c` and `trap.c`):**
- `main.c`: enable `sie.SSIE` and `sstatus.SIE`, remove the test trap,
  add spin loop
- `trap.c`: handle supervisor software interrupt as timer tick, clear
  SSIP, increment counter, add new defines to `riscv.h`

### File layout

```
kernel/
    arch/
        entry.S         <-- UPDATE (timer init + timer_vec handler)
        kernel_vec.S      <-- unchanged
    include/
        riscv.h         <-- UPDATE (add mie/sie/sip/mstatus bit defines,
                                    CLINT addresses)
    main.c              <-- UPDATE (enable interrupts, remove test trap)
    trap.c              <-- UPDATE (handle timer tick)
```

### New definitions for `riscv.h`

We need to add:

```c
/* ---- MIE (Machine Interrupt Enable) bits ---- */
#define MIE_MTIE            (1UL << 7)   /* machine timer */

/* ---- SIE (Supervisor Interrupt Enable) bits ---- */
#define SIE_SSIE            (1UL << 1)   /* supervisor software */
#define SIE_STIE            (1UL << 5)   /* supervisor timer */
#define SIE_SEIE            (1UL << 9)   /* supervisor external */

/* ---- SIP (Supervisor Interrupt Pending) bits ---- */
#define SIP_SSIP            (1UL << 1)   /* supervisor software pending */

/* ---- CLINT memory-mapped registers ---- */
#define CLINT_BASE          0x2000000UL
#define CLINT_MTIMECMP(hart) (CLINT_BASE + 0x4000 + 8 * (hart))
#define CLINT_MTIME         (CLINT_BASE + 0xBFF8)

/* Timer interval: 100,000 ticks = 10ms at 10 MHz */
#define TIMER_INTERVAL      100000UL
```

### Comparison with xv6

| Aspect | xv6 | bobchouOS (Round 2-3) |
|--------|-----|----------------------|
| Timer init code | `timerinit()` in `start.c` (C) | In `entry.S` (assembly) |
| Timer vector | `timervec` in `kernelvec.S` | `timer_vec` in `entry.S` |
| Scratch area | Per-hart `timer_scratch[NCPU][5]` | Single `timer_scratch[5]` (one hart) |
| Timer interval | 1,000,000 (100ms) | 100,000 (10ms) |
| M→S forwarding | SSIP (software interrupt) | SSIP (software interrupt) |
| S-mode handler | `devintr()` → `clockintr()` → `yield()` | `kernel_trap()` → increment ticks |
| Tick counter | `ticks` (protected by `tickslock`) | `ticks` (volatile, no lock needed) |
| Multi-hart | Yes (per-hart scratch, per-hart `mtimecmp`) | No (hart 0 only) |
| On timer tick | `yield()` to preempt process | Increment counter, print every 100 ticks |
| SBI usage | None for timer (direct CLINT access) | None (`-bios none`) |

The key architectural difference: xv6 has its M-mode setup in C
(`start.c` runs in M-mode before `mret`), while bobchouOS does it in
assembly (`entry.S`). Both are valid approaches. xv6 can use C because
it has a stack set up in M-mode. We could do the same, but assembly
keeps our M-mode code self-contained in one file and avoids the need
for a separate M-mode C environment.

---

## Part 10: Understanding the Full Timer Tick Lifecycle

Let's trace a single timer tick from start to finish. The high-level
flow is:

```
CLINT fires  -->  M-mode saves regs, schedules next tick, pokes S-mode
             -->  S-mode saves regs, handles tick, resumes
```

Here are the detailed steps:

```
Step   Task                  Details                               Where
----   -------------------   -----------------------------------   -----------

 T=0   Timer deadline hit    mtime >= mtimecmp[0]                  CLINT hw
                             CLINT asserts MTIP (mip bit 7)

 T=1   M-mode trap entry     CPU sees MTIP pending, mie.MTIE=1,   CPU hw
                             current mode = S (< M)
                             → take M-mode trap
                             mepc ← PC, mcause ← 7|bit63,
                             mstatus: MPIE ← MIE, MIE ← 0,
                             MPP ← S, PC ← mtvec

 T=2   Save M-mode regs      csrrw a0, mscratch, a0               timer_vec
                             sd a1/a2/a3 to scratch

 T=3   Schedule next tick    ld &mtimecmp, old value, interval     timer_vec
                             mtimecmp = mtimecmp + interval
                             (CLINT: mtime < new mtimecmp
                              → MTIP de-asserted)

 T=4   Poke S-mode           li a1, 2                              timer_vec
                             csrs sip, a1 → sip.SSIP = 1

 T=5   Return to S-mode      Restore a1/a2/a3 from scratch         timer_vec
                             csrrw a0, mscratch, a0
                             mret → PC ← mepc, mode ← S,
                                    MIE ← MPIE

 T=6   S-mode trap entry     SSIP pending, sie.SSIE=1,             CPU hw
                             sstatus.SIE=1 → immediate S-trap
                             sepc ← PC, sstatus: SPIE ← SIE,
                             SIE ← 0, SPP ← S, PC ← stvec

 T=7   Save S-mode regs      kernel_vec saves caller-saved regs    kernel_vec.S
                             call kernel_trap

 T=8   Handle timer tick     scause = 0x8...001 → IRQ_S_SOFT      trap.c
                             Clear sip.SSIP
                             ticks++
                             if (ticks % 100 == 0) print
                             return

 T=9   Restore S-mode regs   kernel_vec restores registers         kernel_vec.S
                             sret → PC ← sepc, SIE ← SPIE,
                                    mode ← SPP

 T=10  Resume                S-mode code resumes exactly where     S-mode
                             it was interrupted
```

There are **two trap transitions** in one tick:

1. **S-mode → M-mode** (machine timer interrupt, T=1)
2. **M-mode → S-mode → S-mode trap** (supervisor software interrupt, T=5-6)

This double-trap is the cost of the forwarding trick. The two
round-trips total about 100-150 instructions, which at 1 GHz is roughly
100-150 nanoseconds — about 0.002% of our 10ms tick. Completely
negligible as a performance cost. The real motivation for SSTC isn't
saving nanoseconds — it's eliminating the M-mode dependency entirely:
simpler software (no firmware needed for timers), better for
virtualization (hypervisors don't need to trap every timer tick), and
a cleaner architecture (S-mode manages its own timer without help).

> **Why does the S-mode trap fire at T=6, immediately after `mret`?**
>
> When `timer_vec` does `mret`, the CPU returns to S-mode with SSIP
> pending. The hardware checks: is `sie.SSIE` = 1 and `sstatus.SIE` = 1?
> If yes, the CPU doesn't even execute one S-mode instruction — it
> immediately takes the S-mode trap. The interrupted instruction from
> *before* the M-mode trap is saved in `sepc` (from step T=6), not any
> new instruction.
>
> Technically, the RISC-V spec allows one instruction to execute
> before the pending interrupt is taken (the spec says interrupts are
> taken "before executing the next instruction" but implementations have
> latitude). In practice on QEMU, the trap fires immediately. The exact
> timing doesn't matter for correctness — what matters is that SSIP is
> eventually serviced, and it is.

---

## Part 11: A Note on `wfi` — Wait For Interrupt

Instead of a busy spin loop:

```c
for (;;)
    ;
```

We can use the `wfi` (Wait For Interrupt) instruction:

```c
for (;;)
    asm volatile("wfi");
```

`wfi` tells the CPU "I have nothing to do — sleep until an interrupt
arrives." On real hardware, this saves power. On QEMU, it reduces CPU
usage on the host machine.

The CPU wakes up when any interrupt fires (regardless of whether it's
enabled), executes the trap handler, and then continues after the `wfi`.

> **Is `wfi` privileged?** On RISC-V, `wfi` can be executed in any
> mode (M, S, or U). However, the `mstatus.TW` (Timeout Wait) bit can
> cause `wfi` to trap if executed in a lower privilege mode after a
> timeout. This is used by hypervisors to prevent a guest OS from
> halting a virtual CPU forever. In our simple setup, `wfi` from S-mode
> works fine.
>
> xv6 doesn't use `wfi` in its scheduler — it busy-waits. This is a
> common simplification in teaching OSes. We'll use `wfi` in our spin
> loop because it makes QEMU use less host CPU.

---

## What's Next

After you read this lecture, we'll:

1. **Create the skeleton** — timer setup in `entry.S` (scratch area,
   CLINT programming, `mtvec`, `mie`), `timer_vec` handler, updates
   to `riscv.h`, `main.c` (enable interrupts, remove test trap),
   and `trap.c` (handle timer tick)
2. **You implement the TODOs** — fill in the M-mode timer initialization
   sequence, the `timer_vec` handler, and the S-mode interrupt enable /
   tick handling code
3. **Test** — `make run` should show periodic timer tick output like:
   `timer: 1 seconds`, `timer: 2 seconds`, ... printing once per second

Then we move to Round 2-4: exception handling (better diagnostics for
illegal instructions, page faults, etc.).

---

## Quick Reference

### CLINT register map (QEMU `virt`)

| Address | Register | Size | Description |
|---------|----------|------|-------------|
| `0x200_0000 + 4*hart` | `msip[hart]` | 4B | Software interrupt pending (for IPI — Inter-Processor Interrupt) |
| `0x200_4000 + 8*hart` | `mtimecmp[hart]` | 8B | Timer compare (per-hart deadline) |
| `0x200_BFF8` | `mtime` | 8B | Global time counter (shared) |

### Interrupt enable bits

| Register | Bit | Name | What it enables |
|----------|:---:|------|----------------|
| `mie` | 7 | MTIE | Machine timer interrupt |
| `mie` | 3 | MSIE | Machine software interrupt |
| `mie` | 11 | MEIE | Machine external interrupt |
| `sie` | 1 | SSIE | Supervisor software interrupt |
| `sie` | 5 | STIE | Supervisor timer interrupt |
| `sie` | 9 | SEIE | Supervisor external interrupt |
| `mstatus` | 3 | MIE | M-mode global interrupt enable |
| `sstatus` | 1 | SIE | S-mode global interrupt enable |

### Interrupt pending bits

| Register | Bit | Name | What it means |
|----------|:---:|------|--------------|
| `mip` | 7 | MTIP | Machine timer interrupt pending |
| `mip` | 3 | MSIP | Machine software interrupt pending |
| `mip` | 11 | MEIP | Machine external interrupt pending |
| `sip` | 1 | SSIP | Supervisor software interrupt pending |
| `sip` | 5 | STIP | Supervisor timer interrupt pending |
| `sip` | 9 | SEIP | Supervisor external interrupt pending |

### Timer tick forwarding sequence (our SSIP approach)

| Step | Actor | Action |
|------|-------|--------|
| 1 | CLINT | `mtime >= mtimecmp[hart]` → assert MTIP |
| 2 | CPU | M-mode trap → jump to `mtvec` (`timer_vec`) |
| 3 | `timer_vec` | `mtimecmp += interval` (clear MTIP) |
| 4 | `timer_vec` | `sip.SSIP = 1` (notify S-mode) |
| 5 | `timer_vec` | `mret` → return to S-mode |
| 6 | CPU | SSIP pending → S-mode trap → jump to `stvec` (`kernel_vec`) |
| 7 | `kernel_vec` | Save registers, call `kernel_trap()` |
| 8 | `kernel_trap` | Recognize IRQ_S_SOFT, clear SSIP, `ticks++` |
| 9 | `kernel_vec` | Restore registers, `sret` → resume |

### Scratch area layout (for `timer_vec`)

```
Offset    Contents              Set by
------    --------------------  ---------------------------
  0       (save slot for a1)    timer_vec (at runtime)
  8       (save slot for a2)    timer_vec (at runtime)
 16       (save slot for a3)    timer_vec (at runtime)
 24       &mtimecmp[hartid]     entry.S (at boot, once)
 32       timer interval        entry.S (at boot, once)
```

### New RISC-V instructions and pseudoinstructions in this lecture

| Instruction | Meaning |
|------------|---------|
| `csrs csr, rs` | Set bits: `csr \|= rs` (alias for `csrrs zero, csr, rs`) |
| `csrc csr, rs` | Clear bits: `csr &= ~rs` (alias for `csrrc zero, csr, rs`) |
| `csrrw rd, csr, rs` | Atomic swap: `rd = csr; csr = rs` |
| `wfi` | Wait For Interrupt (halt until interrupt arrives) |
| `mret` | Return from M-mode trap |

### Key CSR writes (summary of what we set and where)

```
In entry.S (M-mode, before mret):
  mscratch  = &timer_scratch         (scratch area pointer)
  mtvec     = &timer_vec             (M-mode trap vector)
  mie      |= MTIE (bit 7)          (enable machine timer interrupt)
  mtimecmp  = mtime + interval       (first deadline, via MMIO)

In kmain() (S-mode):
  stvec     = &kernel_vec            (already done in Round 2-2)
  sie      |= SSIE (bit 1)          (enable supervisor software interrupt)
  sstatus  |= SIE  (bit 1)          (global interrupt enable)
```
