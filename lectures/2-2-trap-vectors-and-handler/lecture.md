# Lecture 2-2: Trap Vectors and the Trap Handler

> **Where we are**
>
> The kernel now boots into S-mode. In Lecture 2-1, we configured
> `mstatus.MPP`, wrote `mepc`, delegated traps, set up PMP, and executed
> `mret`. The CPU dropped to S-mode and landed in `kmain()`. We verified
> this by reading `sstatus`.
>
> But there is a glaring problem: **we have no trap handler.** If
> anything unexpected happens right now — a timer fires, a bad memory
> access, an illegal instruction — the CPU jumps to whatever garbage is
> in `stvec` and crashes silently. We're flying without a safety net.
>
> This lecture fixes that. We'll build the trap infrastructure: an
> assembly stub that saves every register, a C function that inspects
> `scause` and decides what to do, and the wiring that connects them.
> By the end of Round 2-2's code, any trap will land in our handler,
> print diagnostics, and halt cleanly instead of silently crashing.
>
> By the end of this lecture, you will understand:
>
> - What traps are and why the kernel must handle them
> - The three flavors of traps: interrupts, exceptions, and ecalls
> - Every S-mode trap CSR: `stvec`, `sepc`, `scause`, `stval`, `sscratch`
> - The exact sequence of steps the hardware performs when a trap fires
> - What a trap frame is and why all 31 registers must be saved
> - How to write an assembly stub (`kernel_vec.S`) that saves and restores
>   the full register set
> - How to write a C dispatcher (`trap.c`) that reads `scause` and acts
> - How xv6 handles kernel-mode traps (`kernelvec.S` + `kerneltrap()`)
> - How bobchouOS will implement the same thing, simplified for our stage
>
> **xv6 book coverage:** This lecture absorbs Chapter 4 sections 4.1
> (RISC-V trap machinery) and 4.5 (traps from kernel space). It also
> absorbs the Chapter 5 introduction (what drivers and interrupts are,
> top half / bottom half). Sections 4.2-4.4 (user-space traps, syscalls)
> wait until Phase 6 when we have user processes. Section 4.6 (page-fault
> exceptions) waits until Phase 4 when we have page tables.

---

## Part 1: What Are Traps?

### The concept

A **trap** is any event that forces the CPU to stop what it's doing and
transfer control to a designated handler. The xv6 book puts it well:
three kinds of events cause the CPU to "set aside ordinary execution of
instructions and force a transfer of control to special code."

Think of it like a fire alarm in an office building. Everyone is working
normally (executing instructions). The alarm goes off (a trap). Everyone
stops what they're doing and follows the evacuation procedure (the trap
handler). After the situation is resolved, people return to their desks
and resume where they left off.

The key properties of a trap:

1. **Forced** — the running code doesn't choose to trap (for exceptions
   and interrupts). The CPU detects the condition and acts.
2. **Transparent** — ideally, the interrupted code never knows the trap
   happened. It resumes exactly where it left off with all registers
   intact.
3. **Handled in the kernel** — traps always transfer control to
   privileged code. User programs never see raw traps.

### Why the kernel must handle traps

Without a trap handler, the CPU has nowhere to go when something
happens. On RISC-V, if `stvec` contains 0 (or garbage) and a trap
fires, the CPU jumps to address 0 — which is unmapped or contains
random data. The result: silent hang or infinite trap loop.

Every OS — from a tiny embedded RTOS to Linux — must install a trap
handler before enabling any feature that could generate traps. This
includes:

- **Interrupts** — timer ticks, UART data received, disk I/O complete
- **Exceptions** — illegal instructions, page faults, misaligned access
- **System calls** — user programs requesting kernel services (`ecall`)

Our kernel already delegated traps to S-mode (Lecture 2-1), and we're
about to enable timer interrupts (Round 2-3). But first, we need the
safety net.

---

## Part 2: The Three Flavors of Traps

All traps use the same hardware mechanism (save PC, jump to handler),
but they come from three fundamentally different sources. Understanding
the difference is crucial because the handler must respond differently
to each.

### Interrupts (asynchronous)

An **interrupt** is caused by something *external* to the currently
running instruction. The CPU is happily executing code, and a device
says "hey, I need attention." The interrupt has nothing to do with the
instruction that was running — it's purely a timing coincidence.

Examples:
- Timer hardware fires (the deadline you programmed has arrived)
- UART received a byte (someone typed a key)
- Disk controller finished reading a block
- Network card received a packet

Key property: **asynchronous**. The interrupt can arrive between any
two instructions. The interrupted instruction has already completed
successfully — it's the *next* instruction that gets preempted.

```
instruction N     executes normally, completes
                  ← timer interrupt arrives here
instruction N+1   never starts (yet)
                  CPU jumps to trap handler instead
                  ...handler runs...
                  handler does sret
instruction N+1   now executes (as if nothing happened)
```

Because interrupts are asynchronous, the handler must be extra careful:
it can't assume anything about what code was running or what state the
registers are in. It must save *everything*, do its work, restore
*everything*, and return. The interrupted code must not be disturbed.

### Top half and bottom half

The xv6 book introduces an important pattern for device drivers: many
drivers split their work into two parts. The **top half** runs in a
normal process context (e.g., a `read()` system call that asks the UART
for data). The **bottom half** runs in interrupt context (the UART
interrupt handler that collects the incoming byte). The top half starts
an operation and waits; the bottom half responds when the hardware
signals completion.

For example, when you type a key in the QEMU terminal:

```
1. UART hardware receives the byte
2. UART raises an interrupt
3. CPU traps to the kernel's interrupt handler (bottom half)
4. Handler reads the byte from the UART hardware
5. Handler stores it in a buffer
6. Handler wakes up any process waiting for input (top half)
7. Handler returns via sret
8. Later, the waiting process runs and reads from the buffer
```

This split exists because interrupt handlers must be fast — they run
with interrupts disabled, so spending too long in the handler delays
other interrupts. The bottom half does the minimum: grab the data,
store it, signal. The top half does the heavy lifting later, in normal
process context where it's safe to sleep, allocate memory, etc.

We don't have processes yet, so this pattern won't appear in our code
until Phase 5+. But it's good to understand now because it explains
why interrupt handlers in xv6 (and everywhere) are short.

### Are interrupts lost while the handler runs?

When the hardware clears `sstatus.SIE` on trap entry, new interrupts
aren't discarded — they're recorded as **pending** in the `sip`
(Supervisor Interrupt Pending) register. Each S-mode interrupt type
has its own pending bit:

| `sip` bit | Interrupt |
|-----------|-----------|
| 1 (SSIP) | Supervisor software interrupt (used for timer — Round 2-3) |
| 5 (STIP) | Supervisor timer interrupt |
| 9 (SEIP) | Supervisor external interrupt (UART, disk, etc.) |

When `sret` restores SIE, the CPU checks the pending bits and
immediately traps again if any are set. So a delayed interrupt is
delivered — just late.

But pending bits are **flags, not counters**. If the same type fires
twice while the handler is busy, the pending bit is set to 1 both
times — but `1` is still `1`. Multiple events of the same type merge
into one:

```
SIE = 0 (in handler, interrupts disabled)
  device fires   → sip bit set to 1   (pending)
  device fires   → sip bit set to 1   (still 1 — no change)
SIE = 1 (sret restores it)
  CPU sees pending bit → one trap fires
  (second event merged)
```

Does this merging cause data loss? No — because the interrupt is just
a **doorbell**, not the message itself. The real data lives in the
device hardware, and the handler is designed to drain all of it:

**Timer interrupts (bit 1):** The handler reads the current time from
the timer hardware and schedules the next deadline. It doesn't count
how many pending bits fired — it reads the actual clock. Merging a few
timer ticks is harmless; the OS notices the real elapsed time.

**External interrupts (bit 9):** External devices like UART, disk, and
network cards don't connect directly to the CPU. They go through an
interrupt controller called the **PLIC** (Platform-Level Interrupt
Controller) — a hardware block inside the SoC that sits between
devices and CPU cores:

```
                   SoC
  ┌────────────────────────────────────┐
  │                                    │
  │   ┌──────┐  ┌──────┐               │
  │   │ Hart │  │ Hart │               │
  │   │  0   │  │  1   │  ...          │
  │   └──┬───┘  └──┬───┘               │
  │      └────┬────┘                   │
  │      ┌────┴────┐                   │
  │      │  PLIC   │                   │
  │      └────┬────┘                   │
  │       ┌───┼───┬──────┐             │
  │      ┌┴┐ ┌┴┐ ┌┴┐   ┌─┴─┐           │
  │      │U│ │D│ │N│   │...│ ← devices │
  │      │A│ │i│ │e│   │   │           │
  │      │R│ │s│ │t│   │   │           │
  │      │T│ │k│ │ │   │   │           │
  │      └─┘ └─┘ └─┘   └───┘           │
  └────────────────────────────────────┘
```

Each device has a numbered **interrupt source** (e.g., UART = source 10
on QEMU's `virt` machine). When a device needs attention, it asserts
its interrupt line to the PLIC. The PLIC collects all these signals,
applies priority rules, and raises `sip` bit 9 on the appropriate hart.

The PLIC has a **claim/complete protocol** — it acts as a queue, not
just a single flag:

```
1. Device raises interrupt → PLIC records it
2. PLIC asserts sip.SEIP (bit 9) on a hart
3. Hart traps, handler reads PLIC "claim" register
   → PLIC returns the highest-priority source ID (e.g., 10 = UART)
   → Claim clears that source's pending state in the PLIC
4. Handler services the device (reads UART byte, etc.)
5. Handler writes source ID to PLIC "complete" register
   → PLIC knows that source is done, can accept new interrupts from it
```

Multiple devices can be pending simultaneously inside the PLIC. The
handler claims one, handles it, completes it, then checks again. Even
though `sip` bit 9 is a single flag, the PLIC tracks per-device state
internally.

But what about the **same device** firing twice quickly? Between claim
and complete, the PLIC ignores new interrupts from that same source.
So at the PLIC level, a second interrupt from UART merges with the
first. Does that lose data? Still no — because devices have their own
buffers:

| Device | Buffer mechanism |
|--------|-----------------|
| UART | FIFO (typically 16 bytes deep) |
| Disk (virtio) | Descriptor ring |
| Network | Receive ring buffer |

The interrupt means "come check on me." The handler reads **all**
available data from the device, not just one item:

```
keystroke 'a' → UART FIFO: [a]    → UART raises interrupt
keystroke 'b' → UART FIFO: [a, b] → UART raises interrupt (merged)

handler runs:
  claim PLIC → source 10 (UART)
  read UART FIFO → 'a'    (FIFO not empty, read again)
  read UART FIFO → 'b'    (FIFO not empty, read again)
  read UART FIFO → empty   (done)
  complete PLIC → source 10
```

Two keystrokes, one interrupt, zero data lost. The handler drains the
entire FIFO.

Data loss only happens if the **device's own buffer overflows** — e.g.,
the UART FIFO fills up (16 bytes) and a 17th byte arrives before anyone
reads. That's a buffer overrun at the device level, not a flaw in the
interrupt mechanism. The fix is a faster handler (or a bigger FIFO), not
a better PLIC.

So the system is layered: `sip` pending bits are just doorbells.
The PLIC is a per-device queue. The devices have hardware buffers.
Each layer absorbs bursts, and the handler drains everything on each
visit. Merging the *notification* doesn't mean losing the *data*.

### Exceptions (synchronous)

An **exception** is caused by the currently executing instruction
itself. The instruction tried to do something the CPU can't or won't
allow. The exception is *synchronous* — it happens at a deterministic
point, and re-executing the same instruction in the same state will
always produce the same exception.

Examples:
- **Illegal instruction** — the opcode doesn't exist, or it's an M-mode
  instruction executed from S-mode (e.g., reading `mhartid` from S-mode)
- **Page fault** — the instruction accesses a virtual address that isn't
  mapped (or lacks permission)
- **Misaligned access** — a load/store to an address that isn't properly
  aligned
- **Access fault** — PMP denies the memory access
- **Breakpoint** — the `ebreak` instruction (used by debuggers)

Key property: **synchronous** and **precise**. The faulting instruction
is identified exactly — `sepc` points to it. The handler can inspect
the instruction, fix the problem (e.g., map a page), and re-execute it.

```
instruction N     tries to load from unmapped address
                  ← page fault exception fires HERE
                  CPU sets sepc = address of instruction N
                  CPU jumps to trap handler
                  ...handler maps the page...
                  handler sets sepc back to instruction N
                  handler does sret
instruction N     re-executes, this time the page is mapped → succeeds
instruction N+1   continues normally
```

Notice the difference from interrupts: with an exception, `sepc` points
to the *faulting* instruction (it didn't complete). With an interrupt,
`sepc` points to the *next* instruction (the previous one did complete).
This distinction matters when the handler decides whether to re-execute
or skip.

### Environment calls (ecall — a special exception)

The `ecall` instruction is how less-privileged code requests services
from more-privileged code:

| From | To | Purpose |
|------|-----|---------|
| U-mode `ecall` | S-mode trap | User program makes a system call |
| S-mode `ecall` | M-mode trap | Kernel calls firmware (SBI) |

`ecall` is technically an exception (it's synchronous, caused by a
specific instruction), but it's treated specially because it's
*intentional*. The code *wants* to trap — it's not an error.

```
ecall                    causes exception 8 (from U-mode) or 9 (from S-mode)
  CPU sets sepc = address of the ecall instruction
  CPU jumps to trap handler
  handler reads a7 (function number), a0-a5 (arguments)
  handler performs the requested service
  handler sets sepc += 4   ← advance past the ecall
  handler does sret
next instruction          continues after the ecall
```

Notice: the handler advances `sepc` by 4 bytes (one instruction) before
returning. Unlike a page fault, we don't want to re-execute the `ecall`
— the request is done, move on. This `sepc += 4` is a critical detail
that's easy to forget and causes infinite loops if missed.

> **Why 4?** Every RISC-V instruction in the base ISA (RV64I) is exactly
> 4 bytes (32 bits). The compressed extension (RVC / "C" extension) has
> 2-byte instructions, but `ecall` is always 4 bytes. So `sepc + 4`
> reliably points to the next instruction after `ecall`.

### The unified hardware mechanism

Despite these three different sources, the CPU uses the **same hardware
steps** for all of them. The only differences are:

| Property | Interrupt | Exception | ecall |
|----------|-----------|-----------|-------|
| `scause` top bit | 1 | 0 | 0 |
| `scause` code | 1,5,9 (SSI,STI,SEI) | 0-15 (various) | 8 or 9 |
| `sepc` points to | next instruction | faulting instruction | ecall instruction |
| `stval` contains | 0 (usually) | faulting address or instruction | 0 |
| Can be masked? | Yes (`sstatus.SIE`) | No | No |

**Masking** means the kernel can temporarily ignore interrupts by
clearing `sstatus.SIE` (Supervisor Interrupt Enable). While SIE = 0,
an arriving interrupt is not discarded — it's recorded as pending in
`sip` — but the CPU doesn't trap. It keeps executing the current code
and delivers the interrupt later when SIE is set back to 1. This is
how the hardware prevents nested interrupts during a trap handler (it
clears SIE automatically on trap entry — Part 4, step 2).

Exceptions and ecalls cannot be masked. An illegal instruction or a
page fault traps immediately regardless of SIE. There is no way to say
"ignore exceptions for a while." This makes sense: an exception means
the current instruction *cannot complete* — the CPU has no choice but
to transfer control to the handler.

This has a dangerous consequence: if the **handler itself** triggers an
exception (e.g., it dereferences a bad pointer), the CPU traps again
immediately — a **nested trap**. The hardware overwrites `sepc` and
`scause` with the new trap's values, destroying the original context:

```
Original code running
  → exception → CPU saves sepc=A, scause=X, jumps to stvec
  → handler starts running
  → handler hits another exception
  → CPU overwrites sepc=B, scause=Y, jumps to stvec AGAIN
  → same handler restarts from the top
  → original sepc (A) and scause (X) are gone forever
```

If the handler keeps faulting the same way, you get an infinite loop:
trap → handler → trap → handler → ... The CPU spins forever and the
kernel is dead.

### How different OSes handle nested traps

The core problem is always the same: the hardware overwrites
`sepc`/`scause`/`stval` on every trap entry. The solution is always
"save them to memory before they can be clobbered." The difference
is how much recovery logic you build on top.

**bobchouOS (Round 2-2):** No defense. Any exception in the handler
means halt. This is fine for now — our handler is tiny and we can
inspect bugs through QEMU.

**xv6:** Reads `sepc` and `sstatus` into local C variables at the very
top of `kerneltrap()`, before doing anything that could trap. This
protects against *expected* nested traps — for example, a timer
interrupt arriving during `yield()` (which modifies `sepc`). The saved
copies survive the nesting. But for *unexpected* exceptions (a real bug
in the handler), xv6 calls `panic()`. If the handler itself is broken,
no amount of saving helps — the bug repeats on every entry.

**Linux:** Goes much further with several layers of defense:

1. **Full stack frame save.** Linux saves `sepc`/`scause`/`stval` and
   all registers into a stack structure called `pt_regs` immediately on
   entry. Once on the stack, the hardware can overwrite the CSRs freely.

2. **Intentional nested interrupts.** Unlike xv6 (which keeps SIE = 0
   for most of the handler), Linux re-enables interrupts inside the
   handler so high-priority interrupts aren't blocked by lower-priority
   work. Each nesting level pushes another `pt_regs` frame onto the
   kernel stack. This is normal and expected.

3. **Exception fixup table.** Some kernel code *intentionally* accesses
   user-provided pointers that might be invalid (like
   `copy_from_user()`). Instead of panicking on the resulting page
   fault, the kernel looks up the faulting PC in a pre-built table of
   known-risky instructions. If there's a match, it redirects execution
   to a recovery path that returns an error code. If there's no match,
   it's a real kernel bug — panic (called "oops" in Linux).

   ```
   Kernel page fault:
     sepc = faulting instruction address
     → look up sepc in exception fixup table
     → found?     → redirect to recovery path (graceful error)
     → not found? → kernel oops/panic (real bug)
   ```

4. **Stack overflow detection.** If the kernel stack overflows into a
   guard page, the page fault handler detects this specific case and
   switches to a per-CPU overflow stack to print the panic message
   cleanly instead of spiraling into nested faults.

| OS | Strategy | Unexpected exception |
|----|----------|---------------------|
| bobchouOS | No defense | Halt (or infinite loop) |
| xv6 | Save CSRs into locals | `panic()` |
| Linux | Full `pt_regs` save, fixup table, overflow stack | "oops" + recovery if possible, panic if not |

We won't need Linux's sophistication — but it's good to see where the
simple pattern (save CSRs early) scales to when you add user pointers,
preemptive scheduling, and real-world reliability requirements. We'll
revisit the fixup table idea in Phase 6 when we implement
`copy_from_user()`.

---

## Part 3: The S-mode Trap CSRs

Lecture 2-1 listed these CSRs in a reference table. Now we need to
understand each one deeply, because our trap handler code will read and
write all of them.

### `stvec` — Supervisor Trap Vector Base Address

This is the most important trap CSR: it tells the CPU **where to jump**
when a trap occurs.

```
stvec (64 bits):

  Bits    Field    Description
  ─────   ─────    ───────────────────────────────────
  63:2    BASE     Base address of the trap handler (must be 4-byte aligned)
  1:0     MODE     Vectoring mode
```

The MODE field selects how the CPU uses the BASE address:

| MODE | Name | Behavior |
|------|------|----------|
| `00` | Direct | All traps jump to BASE |
| `01` | Vectored | Exceptions jump to BASE; interrupt cause N jumps to BASE + 4*N |
| `10` | — | Reserved |
| `11` | — | Reserved |

**Direct mode** (`MODE = 00`): every trap — whether it's a timer
interrupt, a page fault, or a syscall — jumps to the exact same
address. The handler must read `scause` to figure out what happened.
This is simpler to set up and what most teaching OSes use.

**Vectored mode** (`MODE = 01`): exceptions still jump to BASE, but
each interrupt type jumps to a different address: `BASE + 4 * cause`.
This means interrupt cause 1 (supervisor software interrupt) jumps to
`BASE + 4`, cause 5 (supervisor timer) jumps to `BASE + 20`, and cause
9 (supervisor external) jumps to `BASE + 36`. Each "slot" is 4 bytes
(one instruction) — typically a `j` (jump) to the real handler.

```
Direct mode (MODE = 00):

  Any trap → jump to BASE
  Handler reads scause and branches.

Vectored mode (MODE = 01):

  Exception    → jump to BASE
  Interrupt 1  → jump to BASE + 4
  Interrupt 5  → jump to BASE + 20
  Interrupt 9  → jump to BASE + 36
  (each slot is one 4-byte instruction, usually a jump)
```

> **Why does vectored mode exist?** Performance. In direct mode, the
> handler must read `scause`, check if it's an interrupt, extract the
> code, and branch — that's several instructions before you even start
> handling the interrupt. In vectored mode, the hardware does the
> dispatch for you: each interrupt type lands at its own address, so
> the handler can start work immediately. For a high-performance kernel
> handling thousands of interrupts per second, this saves meaningful
> time.
>
> **Who uses vectored mode?** Linux on RISC-V uses vectored mode. xv6
> uses direct mode (simplicity over performance). bobchouOS will use
> direct mode too.
>
> The alignment requirement comes from the MODE bits living in bits
> 1:0. Since the BASE occupies bits 63:2, the handler address must be
> 4-byte aligned (bottom 2 bits are the mode, not part of the address).
>
> **Stealing low bits from aligned addresses** — this is a recurring
> hardware design pattern. When an address must be N-byte aligned, the
> bottom log2(N) bits are always zero — "free real estate" that the
> hardware repurposes for metadata. We already saw this with `pmpaddr`
> in Lecture 2-1 (NAPOT encoding packs base + size into one register
> by exploiting alignment). `stvec` is the simple version: 4-byte
> alignment frees 2 bits, used for MODE. We'll see it again in Phase 4
> with page table entries — pages are 4096-byte aligned, so the bottom
> 12 bits are free for permission flags (R/W/X, user, valid, etc.).

### `sepc` — Supervisor Exception Program Counter

When a trap fires, the CPU saves the current PC (program counter) into
`sepc`. This is the address the handler can return to via `sret`.

```
sepc (64 bits):

  The full register holds the saved PC.
  On sret, the CPU copies sepc → PC.
```

Important details:

- For **interrupts**: `sepc` = address of the instruction that *would
  have* executed next (the interrupted instruction already completed)
- For **exceptions**: `sepc` = address of the *faulting* instruction
  (it did not complete)
- For **ecall**: `sepc` = address of the `ecall` instruction itself
  (the handler should add 4 before `sret` to skip past it)

The handler can **modify** `sepc` before doing `sret`. This is how
the kernel:
- Advances past `ecall` (add 4)
- Resumes a different process (write a completely different address)
- Retries a faulting instruction after fixing the fault (leave `sepc`
  unchanged)

### `scause` — Supervisor Cause

This register tells the handler *why* the trap occurred.

```
scause (64 bits):

  Bit 63      Interrupt flag: 1 = interrupt, 0 = exception
  Bits 62:0   Exception/interrupt code
```

The handler's first job is reading `scause` and branching:

```c
uint64 cause = csrr(scause);

if (cause & (1UL << 63)) {
    // Interrupt — bits 62:0 are the interrupt code
    uint64 code = cause & 0xff;
    switch (code) {
        case 1:  /* supervisor software interrupt */ break;
        case 5:  /* supervisor timer interrupt */    break;
        case 9:  /* supervisor external interrupt */ break;
    }
} else {
    // Exception — the full value is the exception code
    switch (cause) {
        case 2:  /* illegal instruction */    break;
        case 12: /* instruction page fault */ break;
        case 13: /* load page fault */        break;
        case 15: /* store page fault */       break;
        // ... etc
    }
}
```

We already listed all the cause codes in Lecture 2-1's Quick Reference.
They'll appear again in this lecture's reference section.

> **Why bit 63 and not a separate register?** Packing the interrupt
> flag into the top bit of `scause` is a classic hardware trick. A
> single CSR read gives you both the type and the code. In C, testing
> bit 63 is just checking if the value is negative (signed comparison),
> which many processors can do in one instruction. Two CSR reads would
> be slower and waste a register address.

### `stval` — Supervisor Trap Value

This register provides **extra information** about the trap. What it
contains depends on the trap type:

| Trap type | `stval` contains |
|-----------|-----------------|
| Page fault (load/store/instruction) | The faulting virtual address |
| Illegal instruction | The instruction encoding (on some implementations) |
| Misaligned access | The misaligned address |
| Address misaligned | The target address |
| Everything else | 0 (or implementation-defined) |

For exceptions like page faults, `stval` is essential — it tells the
handler *which* address caused the fault. Without it, the handler would
have to decode the faulting instruction (at `sepc`) to figure out what
address it was trying to access. `stval` saves that work.

For interrupts, `stval` is typically 0 — there's no "faulting address"
for a timer tick.

> **Implementation-defined behavior:** The RISC-V spec says `stval`
> *may* contain the illegal instruction encoding, but implementations
> are allowed to set it to 0 instead. QEMU does provide the instruction
> encoding for illegal instruction exceptions, which is helpful for
> debugging. Real hardware varies.

### `sscratch` — Supervisor Scratch Register

This is a general-purpose scratch register that the trap handler can
use for temporary storage. The hardware doesn't do anything automatic
with it — it's purely for software's convenience.

The typical usage pattern:

```
Before any trap:
  sscratch = address of the trap frame (a save area for registers)

When a trap fires:
  The handler needs to save registers, but all registers contain
  the interrupted code's values. Where do you put them? You need at
  least one register to hold the save-area address, but using any
  register destroys its current value.

  Solution: csrrw (CSR Read-Write) atomically swaps a register
  with sscratch. The handler swaps a0 with sscratch — now a0 holds
  the trap frame address, and sscratch holds the old a0 value.
  The handler saves all other registers to the trap frame, then
  saves the old a0 from sscratch.
```

The `csrrw` (CSR Read-Write) instruction is the key:

```asm
csrrw  a0, sscratch, a0
```

This does `temp = sscratch; sscratch = a0; a0 = temp` — all in one
atomic instruction. After this:
- `a0` = old `sscratch` (the trap frame address)
- `sscratch` = old `a0` (the interrupted code's a0 value, saved for
  later)

> **Do we need `sscratch` for kernel traps?** When a trap occurs while
> the kernel is already running (which is our situation — we don't have
> user processes yet), we already have a valid kernel stack. We can just
> push registers onto the stack. `sscratch` is most useful for
> user-to-kernel transitions where the handler needs to switch from the
> user stack to the kernel stack and has no free register to hold the
> kernel stack address.
>
> xv6's `kernelvec` (the kernel-mode trap vector) doesn't use
> `sscratch` at all — it saves registers directly to the kernel stack.
> xv6's `uservec` (the user-mode trap vector) uses `sscratch` to
> bootstrap the switch from user context to kernel context.
>
> For bobchouOS Round 2-2, we'll follow xv6's `kernelvec` approach:
> save to the stack, no `sscratch` needed.

### The `csrrw` instruction — swap in one shot

We've seen `csrr` (read) and `csrw` (write). `csrrw` is the
read-and-write-simultaneously variant:

```asm
csrrw  rd, csr, rs     # rd = csr; csr = rs   (atomic)
```

There are two more useful variants:

```asm
csrrs  rd, csr, rs     # rd = csr; csr = csr | rs    (set bits)
csrrc  rd, csr, rs     # rd = csr; csr = csr & ~rs   (clear bits)
```

These are handy for toggling individual bits without a read-modify-write
sequence. For example, to disable S-mode interrupts:

```asm
# Clear the SIE bit (bit 1) in sstatus
li    t0, 2           # bit 1
csrrc zero, sstatus, t0   # sstatus &= ~2, discard old value
```

Or using the "immediate" variants (for small 5-bit values):

```asm
csrrci zero, sstatus, 2   # same thing, no register needed
csrrsi zero, sstatus, 2   # set SIE bit (sstatus |= 2)
```

We won't need all of these right now, but they appear in xv6's source
and are useful to recognize.

---

## Part 4: What the Hardware Does When a Trap Fires

This is the core of the trap mechanism. When a trap occurs in S-mode
(or delegated from U-mode), the CPU performs these steps automatically,
in hardware, before executing a single instruction of your handler:

```
Step  Action                                       Why
────  ──────────────────────────────────           ─────────────────────────────
 1    If it's an interrupt and sstatus.SIE = 0,    Respect the "interrupts off"
      ignore it (stay pending, don't trap).         flag. Exceptions always trap.

 2    Clear sstatus.SIE (set it to 0).             Prevent nested interrupts
                                                    while the handler runs.

 3    Copy sstatus.SIE → sstatus.SPIE.             Save the old interrupt-enable
                                                    state so sret can restore it.

 4    Copy current privilege mode → sstatus.SPP.   Remember whether we came from
                                                    U-mode (0) or S-mode (1).

 5    Set privilege mode to S-mode.                 Traps always execute in
                                                    supervisor mode.

 6    Copy PC → sepc.                              Save where we were so we can
                                                    return there.

 7    Write the cause code → scause.               Tell the handler what happened.

 8    Write supplementary info → stval.            Faulting address, bad instruction,
                                                    etc. (0 for most interrupts).

 9    Copy stvec → PC.                             Jump to the handler.

10    Start fetching instructions at the new PC.   Handler begins executing.
```

That's it. The CPU does NOT:
- Save any general-purpose registers (x0-x31)
- Switch to a different stack
- Switch the page table
- Save floating-point registers

"Switch to a different stack" just means writing a new address into
`sp`. There's nothing magical about stacks in RISC-V — the hardware
doesn't know what a "stack" is. `sp` is just a regular register (`x2`)
that software *agrees* to use as the stack pointer. Some other
architectures do switch stacks automatically on trap entry (e.g., x86
can load a new stack pointer from a hardware table called the TSS),
but RISC-V leaves it entirely to software.

For bobchouOS right now, stack switching isn't needed — when a trap
fires, we're already on the `kmain` stack (the one `entry.S` set up
with `la sp, stack_top`). That's the only stack in the entire system.
The trap handler pushes registers onto that same stack. When we have
multiple processes (Phase 5+), each process will have its own kernel
stack, and user-to-kernel traps (Phase 6) will need to switch from
the user stack to the kernel stack — that's where `sscratch` comes in.

All of that is the **software's** responsibility. The hardware does the
absolute minimum to transfer control safely, and the handler does
everything else. This is a deliberate design choice in RISC-V — minimal
hardware, maximum software flexibility.

> **Why so minimal?** The xv6 book explains: "One reason that the CPU
> does minimal work during a trap is to provide flexibility to software;
> for example, some operating systems omit a page table switch in some
> situations to increase trap performance." By not mandating what the
> software must do, RISC-V lets different OS designs make different
> trade-offs. A microkernel that handles many traps per second might
> optimize aggressively; a teaching OS can keep it simple.

### The `sret` instruction — returning from a trap

`sret` reverses the hardware's work:

```
Step  Action
────  ──────────────────────────────────
 1    Copy sstatus.SPIE → sstatus.SIE    Restore interrupt-enable state
 2    Set sstatus.SPIE ← 1               Clean up (known state)
 3    Read sstatus.SPP → target mode      Where to return (U or S)
 4    Set sstatus.SPP ← 0 (User)         Clear after use (security)
 5    Set privilege mode to target mode   Switch back to U or S
 6    Copy sepc → PC                      Jump back to saved address
```

After `sret`, the CPU is back at the instruction that was interrupted
(or the next one, if the handler advanced `sepc`), in the original
privilege mode, with interrupts restored to their pre-trap state.

### Visualizing the full cycle

```
RUNNING CODE (S-mode)              TRAP HANDLER
─────────────────────              ────────────────────
instruction N completes
                                   ┌── Hardware (automatic) ─┐
  ←── trap fires ──────────────→   │ SIE → SPIE, SIE ← 0     │
                                   │ mode → SPP              │
                                   │ PC → sepc               │
                                   │ cause → scause          │
                                   │ info → stval            │
                                   │ stvec → PC              │
                                   └─────────────────────────┘
                                           │
                                           ▼
                                   ┌── Software (your code) ──┐
                                   │ Save all 31 registers    │
                                   │ (assembly stub)          │
                                   │                          │
                                   │ Read scause              │
                                   │ Dispatch to handler      │
                                   │ (C function)             │
                                   │                          │
                                   │ Restore all 31 registers │
                                   │ (assembly stub)          │
                                   │                          │
                                   │ sret                     │
                                   └──────────────────────────┘
                                           │
  ←── resumes here ────────────────────────┘
instruction N+1 executes
```

---

## Part 5: The Trap Frame — Why We Save Registers

### The register save problem

When the trap handler starts executing, all 32 general-purpose
registers (`x0`-`x31`) still contain the interrupted code's values.
The handler needs to use registers too (it has to call functions,
compute things, read CSRs). If it overwrites any register without
saving it first, the interrupted code will see corrupted state when it
resumes.

Solution: **save all registers** to memory before doing anything, and
**restore them all** before `sret`. The memory area where we store them
is called a **trap frame**.

### Which registers to save

RISC-V has 32 integer registers. `x0` (zero) is hardwired to 0 and
never changes, so we save the other 31:

```
Register  ABI name  Caller/callee saved?  Save in trap?
────────  ────────  ────────────────────  ─────────────
x0        zero      (always 0)            No — hardwired
x1        ra        caller-saved          Yes
x2        sp        callee-saved          Yes
x3        gp        (global pointer)      Yes
x4        tp        (thread pointer)      Yes
x5-x7     t0-t2     caller-saved          Yes
x8        s0/fp     callee-saved          Yes
x9        s1        callee-saved          Yes
x10-x11   a0-a1     caller-saved          Yes
x12-x17   a2-a7     caller-saved          Yes
x18-x27   s2-s11    callee-saved          Yes
x28-x31   t3-t6     caller-saved          Yes
```

**Why save ALL of them, not just caller-saved?** The caller/callee
convention is a contract between *functions that call each other*. But
a trap is not a function call — the interrupted code didn't agree to
any calling convention. It might have a value in `s0` that it expects
to survive. If the trap handler (or any function it calls) overwrites
`s0`, the interrupted code is corrupted. So we must save *everything*.

> **Caller-saved vs callee-saved (quick refresher):**
>
> In the RISC-V calling convention, when function A calls function B:
> - **Caller-saved** (a0-a7, t0-t6, ra): function A must save these if
>   it needs them after the call, because B is free to overwrite them
> - **Callee-saved** (s0-s11, sp): function B must preserve these — if B
>   uses them, B must save and restore them
>
> This is an optimization: it splits the work so not every register
> needs saving on every call. But for traps, we can't assume anything
> about the calling convention because we're interrupting arbitrary code.

### Where to save: stack vs dedicated area

There are two common approaches:

**Option A: Save on the kernel stack** — push all registers onto the
current stack, run the handler, pop them back. This is what xv6 does
for kernel traps (`kernelvec`). It's simple: `sp` already points to a
valid kernel stack, so just subtract space and store.

**Option B: Save to a dedicated trap frame struct** — allocate a
per-process (or per-hart) structure in memory, store the address in
`sscratch`, and save registers there. This is what xv6 does for user
traps (`uservec`). It's needed because the user stack is untrusted —
you can't push kernel data onto it.

For bobchouOS Round 2-2, all traps come from kernel code (we have no
user processes). So **option A** (save on the kernel stack) is the right
choice. Clean and simple.

```
Before trap:                After saving:

       ┌──────────┐               ┌──────────┐
       │          │               │          │
       │  stack   │               │  stack   │
       │  data    │               │  data    │
       │          │               │          │
  sp → ├──────────┤          sp → ├──────────┤
       │          │               │ x31 (t6) │  ← saved registers
       │  (free)  │               │ x30 (t5) │     (31 regs x 8 bytes
       │          │               │   ...    │      = 248 bytes)
       │          │               │ x1  (ra) │
       │          │               ├──────────┤
       │          │               │  (free)  │
       └──────────┘               └──────────┘
```

We allocate 256 bytes (32 slots of 8 bytes each — slot 0 is unused
since we skip `x0`, but it keeps the math simple and aligned to 16
bytes as required by the RISC-V calling convention).

---

## Part 6: The Assembly Stub — `kernel_vec.S`

This is the first code that executes when any S-mode trap occurs. Its
job is simple but critical:

1. Save all 31 registers to the stack
2. Call the C trap handler
3. Restore all 31 registers from the stack
4. Execute `sret` to return

### Why assembly?

The C compiler can't help here. When the handler starts, the compiler
doesn't know what registers are "live" (containing important values
from the interrupted code). If the handler's first line was C code, the
compiler might use `t0` for a temporary — overwriting the interrupted
code's `t0` before we saved it. Only hand-written assembly can guarantee
that we save every register before touching any.

xv6's `kernelvec.S` is 50 lines of assembly. Ours (`kernel_vec.S`) will be similar.

### The save sequence

```asm
.globl kernel_vec
.align 4                    # stvec requires 4-byte alignment
kernel_vec:
    # Make room on the stack for 32 slots x 8 bytes = 256 bytes
    addi  sp, sp, -256

    # Save x1-x31 (skip x0, it's always zero)
    sd    x1,   8(sp)       # ra
    sd    x2,  16(sp)       # sp (already decremented, see note below)
    sd    x3,  24(sp)       # gp
    sd    x4,  32(sp)       # tp
    sd    x5,  40(sp)       # t0
    sd    x6,  48(sp)       # t1
    sd    x7,  56(sp)       # t2
    sd    x8,  64(sp)       # s0/fp
    sd    x9,  72(sp)       # s1
    sd    x10, 80(sp)       # a0
    sd    x11, 88(sp)       # a1
    sd    x12, 96(sp)       # a2
    sd    x13,104(sp)       # a3
    sd    x14,112(sp)       # a4
    sd    x15,120(sp)       # a5
    sd    x16,128(sp)       # a6
    sd    x17,136(sp)       # a7
    sd    x18,144(sp)       # s2
    sd    x19,152(sp)       # s3
    sd    x20,160(sp)       # s4
    sd    x21,168(sp)       # s5
    sd    x22,176(sp)       # s6
    sd    x23,184(sp)       # s7
    sd    x24,192(sp)       # s8
    sd    x25,200(sp)       # s9
    sd    x26,208(sp)       # s10
    sd    x27,216(sp)       # s11
    sd    x28,224(sp)       # t3
    sd    x29,232(sp)       # t4
    sd    x30,240(sp)       # t5
    sd    x31,248(sp)       # t6

    # Call the C trap handler
    call  kernel_trap

    # Restore x1-x31
    ld    x1,   8(sp)
    ld    x2,  16(sp)       # NOTE: we will discuss this below
    ld    x3,  24(sp)
    ld    x4,  32(sp)
    ld    x5,  40(sp)
    ld    x6,  48(sp)
    ld    x7,  56(sp)
    ld    x8,  64(sp)
    ld    x9,  72(sp)
    ld    x10, 80(sp)
    ld    x11, 88(sp)
    ld    x12, 96(sp)
    ld    x13,104(sp)
    ld    x14,112(sp)
    ld    x15,120(sp)
    ld    x16,128(sp)
    ld    x17,136(sp)
    ld    x18,144(sp)
    ld    x19,152(sp)
    ld    x20,160(sp)
    ld    x21,168(sp)
    ld    x22,176(sp)
    ld    x23,184(sp)
    ld    x24,192(sp)
    ld    x25,200(sp)
    ld    x26,208(sp)
    ld    x27,216(sp)
    ld    x28,224(sp)
    ld    x29,232(sp)
    ld    x30,240(sp)
    ld    x31,248(sp)

    # Reclaim the stack space
    addi  sp, sp, 256

    # Return from trap
    sret
```

### Why we save `sp` (and why the restore is tricky)

We save `sp` at offset 16. But by the time we execute `sd x2, 16(sp)`,
we've already modified `sp` (the `addi sp, sp, -256` instruction). So
we're saving the *decremented* `sp`, not the original.

Wait — is that a problem? Let's think about it:

- When we restore, `ld x2, 16(sp)` loads the decremented `sp` value
  back into `sp`
- Then `addi sp, sp, 256` adds 256 to get back to the original

Actually, that works. But there's an even cleaner observation: we don't
actually need to restore `sp` from the saved copy at all. Since `sp`
starts at the saved-frame base and we `addi sp, sp, 256` at the end,
`sp` ends up at its original value. The save-and-restore of `sp` is
there for completeness and correctness if the C handler ever changes
`sp` (which it shouldn't, but defensive programming).

> **xv6's approach:** xv6's `kernelvec` saves and restores all
> caller-saved registers (t0-t6, a0-a7, ra) but NOT callee-saved
> registers (s0-s11, sp, gp, tp). Why? Because `kernelvec` calls
> `kerneltrap()` in C, and the C calling convention guarantees that
> `kerneltrap()` preserves callee-saved registers. This optimization
> saves 12 `sd`/`ld` pairs (96 bytes of stack space, ~24 instructions).
>
> The lecture examples above show all 31 registers for clarity, but
> bobchouOS will follow xv6's optimization in practice — only save
> caller-saved registers. The calling convention already guarantees the
> rest.

### The `.align 4` directive

The `.align 4` before `kernel_vec` is critical. Recall that `stvec`
uses bits 1:0 for the MODE field, so the handler address must be
4-byte aligned (bits 1:0 = 0). `.align 4` tells the assembler to
pad with NOPs until the address is a multiple of 2^4 = 16 bytes.

Wait — `.align 4` means 2^4 = 16 bytes? Or 4 bytes? This is a common
source of confusion:

| Assembler | `.align N` means |
|-----------|-----------------|
| GNU as (GAS) on RISC-V | Align to 2^N bytes |
| NASM | Align to N bytes |

With GNU as (which we use), `.align 4` = align to 16 bytes. That's
stricter than the 4-byte minimum `stvec` requires, which is fine —
16-byte alignment is a subset of 4-byte alignment, and it's better for
cache performance anyway.

If you wanted exactly 4-byte alignment, you'd write `.align 2` (2^2 =
4). But `.align 4` (16-byte) is the conventional choice, matching what
xv6 uses.

---

## Part 7: The C Dispatcher — `trap.c`

After saving all registers, the assembly stub calls `kernel_trap()`.
This is a regular C function that reads `scause` and decides what to
do.

### The dispatcher logic

For Round 2-2, our handler is simple — we don't handle any traps yet
(timer interrupts come in Round 2-3, exceptions in Round 2-4). Every
trap is unexpected, so we print diagnostics and halt:

```c
#include "riscv.h"
#include "kprintf.h"

// Called from kernel_vec (assembly stub) when any S-mode trap occurs.
void
kernel_trap(void)
{
    uint64 sepc_val   = csrr(sepc);
    uint64 scause_val = csrr(scause);
    uint64 stval_val  = csrr(stval);
    uint64 sstatus_val = csrr(sstatus);

    // Check that we came from S-mode (SPP bit should be set)
    if ((sstatus_val & SSTATUS_SPP) == 0)
        kprintf("kernel_trap: not from S-mode?\n");

    // Check that interrupts were disabled (SIE should be 0,
    // since the hardware clears it on trap entry)
    if (sstatus_val & SSTATUS_SIE)
        kprintf("kernel_trap: interrupts enabled during trap?\n");

    if (scause_val & SCAUSE_INTERRUPT) {
        // Interrupt
        uint64 code = scause_val & 0xff;
        kprintf("kernel_trap: unexpected interrupt code=%d\n", code);
    } else {
        // Exception
        kprintf("kernel_trap: exception scause=%p sepc=%p stval=%p\n",
                (void *)scause_val, (void *)sepc_val, (void *)stval_val);
    }

    kprintf("kernel_trap: halting.\n");
    for (;;)
        ;
}
```

This is a **stub** — it handles everything by printing and halting.
In later rounds, we'll add real handlers:

| Round | What we add |
|-------|------------|
| 2-3 | Timer interrupt handler (cause 5) |
| 2-4 | Exception handlers (illegal instruction, etc.) |
| Phase 6 | System call handler (cause 8 from U-mode) |

### The sanity checks

The two checks at the top are important for debugging:

1. **SPP check:** If `sstatus.SPP` is 0, the trap came from U-mode. But
   we don't have user processes, so this would mean something is
   seriously wrong. This check catches configuration bugs early.

2. **SIE check:** The hardware clears `sstatus.SIE` on trap entry (step
   2 in Part 4). If SIE is still set, something corrupted `sstatus` —
   another early warning.

These are assert-style checks. In a production OS, they'd be actual
assertions that panic. For now, we print a warning and continue (to
see as much diagnostic output as possible before halting).

---

## Part 8: Wiring It Up — Setting `stvec`

The trap handler only works if `stvec` points to it. We need to:

1. Write `stvec` in `kmain()` before anything that could trap
2. Use direct mode (MODE = 00) since our handler handles all traps

```c
// In kmain(), early in the function:
extern void kernel_vec(void);   // defined in kernel_vec.S
csrw(stvec, (uint64)kernel_vec);
```

Since `kernel_vec` is aligned to 16 bytes (`.align 4`), the bottom 2
bits of its address are 0, which means MODE = 00 (direct). We don't
need to set the mode bits explicitly — the alignment gives us direct
mode for free.

> **When exactly should we set `stvec`?** As early as possible in
> `kmain()`. Before `stvec` is set, any trap will jump to address 0
> (or whatever garbage was in `stvec`). After setting it, traps land
> safely in our handler.
>
> In xv6, `stvec` is set to `kernelvec` during `main()` initialization
> (specifically in `trapinithart()`). Each hart must set its own `stvec`
> because trap CSRs are per-hart. Since we only run on hart 0, we do it
> once.

---

## Part 9: How xv6 Handles Kernel Traps

Let's look at xv6's implementation. xv6 has two separate trap paths:
one for traps from user space (`uservec` / `usertrap` / `userret`)
and one for traps from kernel code (`kernelvec` / `kerneltrap`). We
only care about the kernel path for now.

### `kernelvec.S` — xv6's kernel trap vector

xv6's `kernelvec` follows the same pattern as Part 6: `addi sp` to
make room, `sd` the registers, `call kerneltrap`, `ld` them back,
`addi sp` to reclaim, `sret`. We won't repeat the full listing here.
The key difference from Part 6's "save everything" example: xv6 only
saves **caller-saved registers** (ra, t0-t6, a0-a7) — the optimization
noted in Part 6. The C calling convention guarantees that `kerneltrap()`
preserves callee-saved registers (s0-s11, sp, gp, tp), so saving them
is redundant. bobchouOS will follow this same optimization.

### `kerneltrap()` — xv6's kernel trap C handler

```c
// kernel/trap.c — xv6's kernel trap handler (simplified)

void
kerneltrap()
{
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    if (scause & (1L << 63)) {
        // Interrupt
        int which_dev = devintr();
        if (which_dev == 0)
            goto unexpected;

        // Timer interrupt: yield the CPU
        if (which_dev == 2 && myproc() != 0
            && myproc()->state == RUNNING)
            yield();
    } else {
        // Exception in kernel = always fatal in xv6
        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", sepc, r_stval());
        panic("kerneltrap");
    }

    // Restore sepc and sstatus (kerneltrap may have called yield,
    // which modifies these CSRs)
    w_sepc(sepc);
    w_sstatus(sstatus);

unexpected:
    ;
}
```

Key observations:

1. **Kernel exceptions are fatal.** This is `kerneltrap()` — it only
   handles traps from S-mode (kernel code). If the kernel itself hits a
   page fault or illegal instruction, something is seriously wrong, so
   xv6 panics. User-mode traps (including ecalls / syscalls) go through
   a completely separate path (`uservec` → `usertrap()`) which does NOT
   panic — it dispatches ecalls to `syscall()` and kills the offending
   process for other exceptions. We'll build that path in Phase 6.
   And what about S-mode `ecall` (the kernel calling firmware)? That's
   exception code 9, which is hardwired to NOT delegate — it goes
   straight to M-mode and is handled by OpenSBI firmware, never by
   `kerneltrap()`.

2. **`devintr()` dispatches device interrupts.** It checks `scause`,
   talks to the PLIC (interrupt controller), and returns which device
   interrupted. We'll build this incrementally in later rounds.

3. **Timer interrupts cause `yield()`.** This is how xv6 preempts
   processes — every timer tick is an opportunity to switch to another
   process. We don't have a scheduler yet, so Round 2-3 will just count
   ticks.

4. **`sepc` and `sstatus` are saved and restored.** Why? Because
   `yield()` can context-switch to another process, which might modify
   these CSRs. When we return from `yield()`, we need the original
   values. This is a subtle detail that won't matter for us until
   Phase 5 (scheduling), but it's good to notice.

---

## Part 10: How bobchouOS Will Do It

### The plan

We need three things:

1. **`kernel/arch/kernel_vec.S`** — assembly stub (save regs, call C,
   restore regs, `sret`)
2. **`kernel/trap.c`** — C dispatcher (read `scause`, print
   diagnostics, halt)
3. **Updates to existing files:**
   - `kernel/include/riscv.h` — add `scause`, `stvec`, etc. CSR
     definitions
   - `kernel/main.c` — set `stvec` to point to `kernel_vec`

### File layout

```
kernel/
    arch/
        entry.S         ← existing (M→S switch)
        kernel_vec.S      ← NEW (trap vector assembly stub)
    include/
        riscv.h         ← UPDATE (add trap CSR definitions)
    trap.c              ← NEW (C trap dispatcher)
    main.c              ← UPDATE (set stvec)
```

### New CSR definitions for `riscv.h`

We need to add macros for the trap-related CSRs that our C code will
read:

```c
/* ---- scause bit definitions ---- */
#define SCAUSE_INTERRUPT    (1UL << 63)

/* Interrupt cause codes (scause value when bit 63 = 1)
 * IRQ = "Interrupt Request" — a term from 1970s Intel 8259 hardware
 * that stuck as the universal prefix for interrupt identifiers.
 * EXC = "Exception" — synchronous faults caused by the instruction. */
#define IRQ_S_SOFT      1       /* supervisor software interrupt */
#define IRQ_S_TIMER     5       /* supervisor timer interrupt */
#define IRQ_S_EXT       9       /* supervisor external interrupt */

/* Exception cause codes (scause value when bit 63 = 0) */
#define EXC_INST_MISALIGN   0
#define EXC_INST_ACCESS     1
#define EXC_ILLEGAL_INST    2
#define EXC_BREAKPOINT      3
#define EXC_LOAD_MISALIGN   4
#define EXC_LOAD_ACCESS     5
#define EXC_STORE_MISALIGN  6
#define EXC_STORE_ACCESS    7
#define EXC_ECALL_U         8
#define EXC_ECALL_S         9
#define EXC_INST_PAGE       12
#define EXC_LOAD_PAGE       13
#define EXC_STORE_PAGE      15
```

These named constants make the C handler readable:

```c
if (scause == (SCAUSE_INTERRUPT | IRQ_S_TIMER)) {
    // handle timer interrupt
}
```

Instead of the cryptic:

```c
if (scause == 0x8000000000000005) {
    // what is this? you have to look up the number
}
```

### Testing the handler

After implementing everything, we can test by deliberately triggering
a trap. The easiest way: try to read an M-mode CSR from S-mode.

```c
// In kmain(), after setting stvec:
kprintf("testing trap handler...\n");

// This will cause an illegal instruction exception (cause 2):
// reading mhartid requires M-mode, but we're in S-mode.
uint64 x = csrr(mhartid);
```

Expected output:

```
testing trap handler...
kernel_trap: exception scause=0x0000000000000002 sepc=0x800xxxxx stval=0x...
kernel_trap: halting.
```

`scause = 2` is "illegal instruction" — exactly right. The test code
tries to execute `csrr ..., mhartid`, the CPU raises an illegal
instruction exception because `mhartid` is an M-mode CSR, the trap
fires, our handler catches it, prints the diagnostics, and halts. This
confirms the entire pipeline works: `stvec` → `kernel_vec` → save regs
→ `kernel_trap()` → print → halt.

> **Important:** The test trap (`csrr mhartid`) will cause the kernel
> to halt. This is expected for Round 2-2 — we're only building the
> infrastructure. In Round 2-4, we'll improve the handler to do
> something useful with exceptions (print a detailed report and either
> skip the instruction or panic with a backtrace).
>
> For the skeleton commit, we'll include the test. For the "real"
> kernel, we'll remove it once we're confident the handler works and
> move on to Round 2-3 (timer interrupts).

---

## Part 11: Comparing bobchouOS and xv6

| Aspect | xv6 | bobchouOS (Round 2-2) |
|--------|-----|----------------------|
| Kernel trap vector | `kernelvec.S` | `kernel_vec.S` |
| Registers saved | Caller-saved only (ra, t0-t6, a0-a7) | Same optimization |
| Save location | Kernel stack | Kernel stack (`kmain` stack) |
| Uses `sscratch`? | No (kernel traps only) | No |
| `stvec` mode | Direct (MODE = 00) | Direct (MODE = 00) |
| C handler | `kerneltrap()` in `trap.c` | `kernel_trap()` in `trap.c` |
| Kernel exceptions | `panic()` | Print and halt |
| User exceptions | `usertrap()` → kill process or `syscall()` | Not yet (Phase 6) |
| S-mode `ecall` | Goes to M-mode (OpenSBI, provided by QEMU) | No SBI (`-bios none`); Round 2-3 handles timer directly |
| Interrupt dispatch | `devintr()` → PLIC | Print "unexpected" and halt |
| Timer interrupt | `yield()` | Not yet (Round 2-3) |
| User trap path | `uservec`/`usertrap`/`userret` | Not yet (Phase 6) |
| `sepc`/`sstatus` save | Yes (for `yield()`) | Not needed yet |

The architecture is the same. xv6 has a more complete handler because
it has a scheduler, PLIC driver, and user processes. We're building up
to that level incrementally.

---

## What's Next

After you read this lecture, we'll:

1. **Create the skeleton** — `kernel_vec.S` and `trap.c` with TODO markers,
   plus `riscv.h` updates and `main.c` wiring
2. **You implement the TODOs** — fill in the assembly save/restore
   sequence and the C dispatcher logic
3. **Test** — `make run` should show the test trap being caught:
   `kernel_trap: exception scause=0x... sepc=0x... stval=0x...`

Then we move to Round 2-3: programming the CLINT hardware to generate
timer interrupts, and extending the handler to actually *handle* them
instead of halting.

---

## Quick Reference

### Trap CSR summary

| CSR | Purpose | Who writes | Who reads |
|-----|---------|-----------|-----------|
| `stvec` | Handler address + mode | Software (kernel init) | Hardware (on trap) |
| `sepc` | Saved PC | Hardware (on trap) | Software (handler, `sret`) |
| `scause` | Trap cause | Hardware (on trap) | Software (handler) |
| `stval` | Supplementary value | Hardware (on trap) | Software (handler) |
| `sscratch` | Scratch register | Software | Software |
| `sstatus` | Interrupt enable, SPP | Hardware + software | Both |

### `scause` encoding

```
Bit 63 = 1 → Interrupt     Bit 63 = 0 → Exception
──────────────────────     ─────────────────────────
Code 1: S software int     Code 0: Instruction misaligned
Code 5: S timer int        Code 1: Instruction access fault
Code 9: S external int     Code 2: Illegal instruction
                           Code 3: Breakpoint
                           Code 4: Load misaligned
                           Code 5: Load access fault
                           Code 6: Store misaligned
                           Code 7: Store access fault
                           Code 8: Ecall from U-mode
                           Code 9: Ecall from S-mode
                           Code 12: Instruction page fault
                           Code 13: Load page fault
                           Code 15: Store page fault
```

### Hardware trap sequence (S-mode)

| Step | Action |
|------|--------|
| 1 | If interrupt and `SIE=0`, stay pending (don't trap) |
| 2 | `SPIE ← SIE`; `SIE ← 0` |
| 3 | `SPP ← current_mode` |
| 4 | `sepc ← PC` |
| 5 | `scause ← cause_code` |
| 6 | `stval ← supplementary_value` |
| 7 | Mode ← Supervisor |
| 8 | `PC ← stvec` (direct) or `stvec + 4*cause` (vectored) |

### `sret` sequence

| Step | Action |
|------|--------|
| 1 | `SIE ← SPIE`; `SPIE ← 1` |
| 2 | Mode ← `SPP`; `SPP ← 0` (User) |
| 3 | `PC ← sepc` |

### New RISC-V instructions in this lecture

| Instruction | Meaning |
|------------|---------|
| `sret` | Return from S-mode trap: restore mode and jump to `sepc` |
| `csrrw rd, csr, rs` | Atomic swap: `rd = csr; csr = rs` |
| `csrrs rd, csr, rs` | Atomic set bits: `rd = csr; csr \|= rs` |
| `csrrc rd, csr, rs` | Atomic clear bits: `rd = csr; csr &= ~rs` |
| `sd rs, offset(base)` | Store doubleword (8 bytes) to memory |
| `ld rd, offset(base)` | Load doubleword (8 bytes) from memory |

### `stvec` MODE field

| MODE (bits 1:0) | Name | Behavior |
|-----------------|------|----------|
| `00` | Direct | All traps → BASE |
| `01` | Vectored | Exceptions → BASE; Interrupt N → BASE + 4*N |

### Register save slots (stack frame layout)

```
Offset    Register    ABI name
──────    ────────    ────────
  0       (unused)    x0/zero
  8       x1          ra
 16       x2          sp
 24       x3          gp
 32       x4          tp
 40       x5          t0
 48       x6          t1
 56       x7          t2
 64       x8          s0/fp
 72       x9          s1
 80       x10         a0
 88       x11         a1
 96       x12         a2
104       x13         a3
112       x14         a4
120       x15         a5
128       x16         a6
136       x17         a7
144       x18         s2
152       x19         s3
160       x20         s4
168       x21         s5
176       x22         s6
184       x23         s7
192       x24         s8
200       x25         s9
208       x26         s10
216       x27         s11
224       x28         t3
232       x29         t4
240       x30         t5
248       x31         t6
```
