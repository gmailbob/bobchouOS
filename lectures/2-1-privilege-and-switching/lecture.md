# Lecture 2-1: Privilege Modes and the M-to-S Switch

> **Where we are**
>
> Phase 1 is done. The kernel boots on QEMU, entry.S parks extra harts,
> zeros BSS, sets up the stack, and jumps to kmain(). We have a UART
> driver (polling I/O) and kprintf with formatted output. The kernel
> prints diagnostics and halts.
>
> But there's a problem: **everything runs in M-mode** (Machine mode).
> That's the highest privilege level — the CPU lets us do anything. This
> was fine for "hello world," but a real OS kernel runs in **S-mode**
> (Supervisor mode). This lecture explains why and shows exactly how to
> make the switch.
>
> By the end of this lecture, you will understand:
>
> - Why an OS must not run in M-mode
> - The RISC-V privilege hierarchy in detail (expanding on 1-1's preview)
> - Every CSR field involved in the M→S switch
> - The `mret` trick — how to fake a trap return to drop privilege
> - Trap delegation — routing traps to S-mode so the kernel handles them
> - Physical Memory Protection (PMP) — unlocking RAM for S-mode
> - How xv6's `start.c` implements all of the above
> - How bobchouOS will do the same in `entry.S`
>
> **xv6 book coverage:** This lecture absorbs Chapter 2, sections 2.2
> (privilege modes) and 2.6 (starting xv6). It also previews Chapter 4,
> section 4.1 (RISC-V trap machinery) — just enough to understand why
> delegation matters. The full trap story comes in Lecture 2-2.

---

## Part 1: Why Leave M-mode?

### The status quo

Right now, our boot path looks like this:

```
QEMU starts all harts at 0x80000000
        │
        ▼
  entry.S (M-mode)
  • Park non-boot harts
  • Zero BSS
  • Set up stack
  • call kmain
        │
        ▼
  kmain() (still M-mode)
  • uart_init()
  • kprintf(...)
  • halt
```

Everything works. We can read and write any CSR, access any memory
address, and touch any hardware register. Why change anything?

### The problem with staying in M-mode

Three reasons, each more important than the last:

**1. No isolation is possible.**

The whole point of an OS is to protect programs from each other. RISC-V
provides this through **page tables** — a mechanism that controls which
memory each program can see. But page tables are an S-mode feature
(controlled by the `satp` CSR). M-mode has no concept of "this address
is forbidden" — it can see everything, always.

If our kernel stays in M-mode and later runs user programs, those
programs would need to be in... what mode? There's no mode below M-mode
that still works without S-mode infrastructure. The privilege hierarchy
is designed as M → S → U, and skipping S breaks the model.

**2. The trap model doesn't work right.**

When something goes wrong (illegal instruction, page fault) or a device
needs attention (UART received data, timer fired), the CPU raises a
**trap**. Traps need a handler — code that decides what to do.

In the RISC-V privilege design, traps from U-mode go to S-mode (the
kernel handles them). But all traps go to M-mode by default. If the
kernel IS running in M-mode, there is no separation between "kernel trap
handling" and "firmware trap handling." You lose the clean layering:

```
Intended design:              What we have now:

U-mode (user programs)        (nothing)
    │  traps go up                │
    ▼                             ▼
S-mode (kernel)               M-mode (everything mashed together)
    │  some traps go up
    ▼
M-mode (firmware)
```

**3. It's not how real hardware works.**

On real RISC-V boards (not QEMU with `-bios none`), firmware like
OpenSBI runs in M-mode and provides services to the kernel. The kernel
runs in S-mode. If we write our kernel assuming M-mode, it won't work
on real hardware without major changes.

By switching to S-mode now, our kernel code will be structurally correct
for all future phases — page tables, trap handling, user programs, and
eventually running on real hardware.

### The plan

We need to modify `entry.S` to do these extra steps before calling
`kmain()`:

```
entry.S (M-mode):
  1. Park non-boot harts          ← already done
  2. Zero BSS                     ← already done
  3. Set up stack                 ← already done
  ─── NEW ──────────────────────────────────
  4. Configure mstatus.MPP = Supervisor
  5. Write kmain address into mepc
  6. Delegate traps to S-mode
  7. Disable paging (satp = 0)
  8. Set up PMP (unlock all memory for S-mode)
  9. Execute mret → CPU drops to S-mode, jumps to kmain
  ──────────────────────────────────────────
```

After `mret`, `kmain()` runs in S-mode. The rest of the kernel never
touches M-mode again (except for timer interrupts in Round 2-3 — we'll
explain why then).

---

## Part 2: The Privilege Hierarchy — Going Deeper

Lecture 1-1 introduced the three modes and the building analogy. Now we
need to understand the details — exactly which CSRs exist at each level,
how mode transitions work, and what each mode can and can't do.

### Privilege encodings

The privilege level is encoded in 2 bits throughout the RISC-V ISA
(Instruction Set Architecture — the spec that defines every instruction,
register, and behavior a RISC-V CPU supports):

| Encoding | Level | Name |
|----------|-------|------|
| `00` | 0 | U-mode (User) |
| `01` | 1 | S-mode (Supervisor) |
| `10` | 2 | (Reserved — not used) |
| `11` | 3 | M-mode (Machine) |

These 2-bit encodings appear inside CSRs (like the MPP field in
`mstatus`) and in the CPU's internal privilege state. Encoding `10` is
reserved for future use — possibly a "Hypervisor" mode for
virtualization. For bobchouOS, we only care about `00`, `01`, and `11`.

### CSR address space encodes privilege

Here's a neat design detail: the CSR **address** itself tells you what
privilege is needed to access it. RISC-V defines 4096 CSR addresses
(12-bit), and bits 9:8 of the address encode the minimum privilege:

```
CSR address: 12 bits
              ┌──────────────┐
  bit 11:10   │  read/write  │  00 = R/W, 11 = read-only
  bit  9:8    │  privilege   │  00 = U, 01 = S, 10 = reserved, 11 = M
  bit  7:0    │  register ID │
              └──────────────┘
```

For example:

| CSR | Address (hex) | Bits 9:8 | Min privilege |
|-----|--------------|----------|---------------|
| `sstatus` | `0x100` | `01` | S-mode |
| `stvec` | `0x105` | `01` | S-mode |
| `mstatus` | `0x300` | `11` | M-mode |
| `mtvec` | `0x305` | `11` | M-mode |
| `mhartid` | `0xF14` | `11` | M-mode (read-only) |

This means the CPU doesn't need a lookup table to check access — it just
compares the CSR address bits against the current privilege level. If the
code's privilege is lower than the CSR requires, the instruction raises
an illegal instruction exception. Simple and fast.

> **Why this matters for us:** After we switch to S-mode, any attempt to
> read an M-mode CSR (like `mhartid`) will cause an exception. Right now,
> we have no trap handler, so such an exception would crash the CPU
> silently. In Round 2-4, we'll add an exception handler that prints
> diagnostics, and at that point we can deliberately test this — read
> `mhartid` from S-mode and watch the handler catch it.

### What S-mode can and can't do

Once we're in S-mode, here's what changes:

| Capability | M-mode | S-mode |
|-----------|--------|--------|
| Read/write M-mode CSRs (`mstatus`, `mepc`, ...) | Yes | **No** — illegal instruction |
| Read/write S-mode CSRs (`sstatus`, `sepc`, ...) | Yes | Yes |
| Execute `mret` | Yes | **No** — illegal instruction |
| Execute `sret` | Yes | Yes |
| Configure page tables (`satp`) | Yes | Yes |
| Access all physical memory | Yes | **Only if PMP allows it** |
| Respond to interrupts | All | Only delegated ones |

The last two points are important and we'll cover them in detail:
- **PMP** (Physical Memory Protection) controls what memory S-mode can
  access — we must configure it before switching
- **Delegation** controls which traps S-mode handles — we must set that
  up too

### The S-mode CSRs we'll use

Here's the full set of S-mode CSRs we'll encounter across Phase 2. Don't
memorize them now — this is a reference table to come back to:

| CSR | Name | Purpose | Which round |
|-----|------|---------|-------------|
| `sstatus` | Supervisor Status | Interrupt enable (SIE), previous privilege (SPP) | 2-1 (verify), 2-2 |
| `stvec` | Supervisor Trap Vector | Address of S-mode trap handler | 2-2 |
| `sepc` | Supervisor Exception PC | PC saved when trap occurred | 2-2 |
| `scause` | Supervisor Cause | Reason for the trap (interrupt vs exception, code) | 2-2, 2-4 |
| `stval` | Supervisor Trap Value | Extra info (faulting address, bad instruction) | 2-4 |
| `sscratch` | Supervisor Scratch | Scratch register for trap handler to use | 2-2 |
| `sie` | Supervisor Interrupt Enable | Per-type interrupt enable bits | 2-3 |
| `sip` | Supervisor Interrupt Pending | Which interrupts are waiting | 2-3 |
| `satp` | Supervisor Address Translation | Page table base register | Phase 4 (disabled for now) |

---

## Part 3: The `mstatus` Register — The Control Center

The `mstatus` register is the most important CSR for the M→S switch.
It's a 64-bit register packed with bit fields that control many aspects
of CPU behavior. We only need a few fields right now.

### Layout (simplified)

Here are the fields relevant to Phase 2. The full register has more
fields (for floating-point, virtualization, etc.), but we'll ignore
those:

```
mstatus (64 bits):

  Bit(s)   Field    Description
  ─────    ─────    ──────────────────────────────────────
    63      SD       Summary Dirty (FP/vector state; ignore)
   12:11    MPP      Machine Previous Privilege (2 bits)
    7       MPIE     Machine Previous Interrupt Enable (1 bit)
    3       MIE      Machine Interrupt Enable (1 bit)
    8       SPP      Supervisor Previous Privilege (1 bit)
    5       SPIE     Supervisor Previous Interrupt Enable (1 bit)
    1       SIE      Supervisor Interrupt Enable (1 bit)

  Encoding for MPP (bits 12:11):
    00 = User        ← mret will switch to U-mode
    01 = Supervisor  ← mret will switch to S-mode (what we want)
    11 = Machine     ← mret will stay in M-mode

  Encoding for SPP (bit 8):
    0 = User         ← sret will switch to U-mode
    1 = Supervisor   ← sret will stay in S-mode
```

```
Bit layout (relevant bits only, gaps between fields omitted):

  63          12  11    8    7    5    3    1    0
 ┌────────────┬──────┬────┬────┬────┬────┬────┬────┐
 │    ...     │ MPP  │SPP │MPIE│SPIE│MIE │SIE │ ...│
 │            │ (2b) │(1) │(1) │(1) │(1) │(1) │    │
 └────────────┴──────┴────┴────┴────┴────┴────┴────┘

(Bits 10:9, 6, 4, 2, 0 exist between these fields but are reserved
or used by other features. See the table above for exact positions.)
```

### MPP — the mode-switch lever

**MPP** (Machine Previous Privilege, bits 12:11) is the field that makes
the M→S switch possible. Here's how it works:

**During a normal trap to M-mode:**
1. Something causes a trap (exception, interrupt, `ecall`)
2. CPU automatically saves the current privilege level into `mstatus.MPP`
3. CPU switches to M-mode
4. CPU jumps to the address in `mtvec`

**During `mret`:**
1. CPU reads `mstatus.MPP`
2. CPU switches to the privilege level encoded in MPP
3. CPU sets MPP to `00` (User) — so a second `mret` without a new trap
   doesn't stay in the same mode
4. CPU jumps to the address in `mepc`

The boot trick: we manually write `01` (Supervisor) into MPP before
executing `mret`. The CPU doesn't verify that a trap actually happened —
it just reads MPP and does what it says.

### MIE and MPIE — interrupt enable bits

**MIE** (Machine Interrupt Enable, bit 3) is the global interrupt
on/off switch for M-mode. If MIE = 0, no interrupts are delivered to
M-mode (they remain pending until MIE is set).

**MPIE** (Machine Previous Interrupt Enable, bit 7) saves the old MIE
value during a trap. When `mret` executes, the CPU restores MIE from
MPIE. This ensures that if interrupts were enabled before the trap, they
get re-enabled on return.

```
Trap to M-mode:        mret:
  MPIE ← MIE            MIE ← MPIE
  MIE  ← 0              MPIE ← 1
  (interrupts off)      (interrupts restored)
```

**Why `MIE ← 0` on trap entry?** This prevents a trap from being
interrupted by another trap. Imagine: a timer interrupt fires, the
handler starts saving registers, then a second timer interrupt arrives
mid-save — it would overwrite `mepc` and `mcause` before the first
handler had a chance to read them. Setting MIE = 0 ensures the handler
runs to completion. The handler re-enables interrupts when it's ready —
either explicitly (writing MIE = 1) or implicitly via `mret`, which
restores MIE from MPIE.

**Why `MPIE ← 1` on `mret`?** This is purely defensive cleanup. The
value will be overwritten by the next trap entry (`MPIE ← MIE`), so it
doesn't matter in practice. The spec sets it to 1 so the field is in a
known state — same philosophy as clearing MPP to `00` (User) after
`mret`. Stale values in control registers are a source of subtle bugs,
so the hardware cleans up after itself.

Note that MIE/MPIE control **M-mode** interrupts only. S-mode has its
own interrupt enable bit: `sstatus.SIE` (bit 1). After `mret` drops us
to S-mode, the MIE value no longer matters — what matters is `sstatus.SIE`,
which defaults to 0 (disabled) at reset. We'll enable it explicitly
when we're ready (Round 2-3, after setting up the trap handler).

> **SIE, SPIE, SPP** — the S-mode equivalents work identically but for
> S-mode traps (`sret` instead of `mret`):
>
> ```
> Trap to S-mode:        sret:
>   SPIE ← SIE            SIE ← SPIE
>   SIE  ← 0              SPIE ← 1
>   (S-mode ints off)     (S-mode ints restored)
> ```
>
> The `sstatus` CSR is actually a **view** of `mstatus` — reading
> `sstatus` gives you only the S-mode fields (SIE, SPIE, SPP, etc.)
> while hiding M-mode fields. From S-mode's perspective, `sstatus` is
> the only status register it can see. From M-mode, you can read both
> `mstatus` (full) and `sstatus` (restricted view).

### Bit manipulation in assembly

To set MPP = 01 (Supervisor) in `mstatus`, we need to:

1. Read the current `mstatus` value
2. Clear bits 12:11 (set them to 00)
3. Set bit 11 (making the field 01)
4. Write the result back

In assembly:

```asm
    # Read mstatus
    csrr  t0, mstatus

    # Clear MPP field: bits 12:11
    # The mask for bits 12:11 is (3 << 11) = 0x1800
    # We want to AND with the complement: ~0x1800
    li    t1, ~(3 << 11)
    and   t0, t0, t1

    # Set MPP = 01 (Supervisor): set bit 11
    li    t1, (1 << 11)
    or    t0, t0, t1

    # Write mstatus back
    csrw  mstatus, t0
```

This is a common pattern for CSR bit fields: read, mask, set, write.
You'll see it repeatedly in OS code.

> **Why not just write the whole register?**
>
> You might wonder: why read-modify-write instead of just writing a known
> value? Because `mstatus` has many other fields (SD, FS, XS, etc.) that
> we don't want to disturb. Some of those fields might have been set by
> the hardware or by earlier code. Blindly writing the entire register
> could break things. The read-modify-write pattern only changes the bits
> we care about and preserves everything else.

### The `mepc` register

`mepc` (Machine Exception Program Counter) stores the address that `mret`
jumps to. During a normal trap, the CPU saves the interrupted PC here
automatically. At boot, we write it manually:

```asm
    la    t0, kmain
    csrw  mepc, t0
```

After `mret`, the CPU jumps to whatever address is in `mepc` — in our
case, `kmain`. Combined with MPP = Supervisor, we get: jump to `kmain`
AND switch to S-mode. That's the complete trick.

---

## Part 4: Trap Delegation — Letting S-mode Handle Its Own Traps

Recall from Lecture 1-1 that there are three types of traps: exceptions,
interrupts, and system calls (`ecall`). `ecall` is just a type of
exception — cause codes 8/9/11 in `medeleg`, one per privilege level.
All three use the **same hardware mechanism**: save PC to `xepc`, save
cause to `xcause`, jump to `xtvec`. The CPU treats them identically.

### The problem

By default in RISC-V, **all traps go to M-mode.** If we switch the
kernel to S-mode but don't set up delegation, here's what happens when
a timer interrupt fires:

```
S-mode kernel running
      │  (timer interrupt)
      ▼
CPU switches to M-mode
Jumps to mtvec
      │
      ▼
??? (no M-mode handler — crash)
```

Even if we had an M-mode handler, it couldn't just swallow the interrupt
— the S-mode kernel needs to know the timer fired (to switch processes,
update a tick counter, etc.). So M-mode would have to forward the
interrupt to S-mode somehow, e.g., by raising a software interrupt. Every
interrupt becomes a two-step: M-mode catches it, then M-mode notifies
S-mode. That's an unnecessary round-trip.

(This forwarding pattern is actually unavoidable for timer interrupts —
the RISC-V spec says machine timer interrupts always go to M-mode, no
matter what. We'll deal with that in Round 2-3. But for everything
else, delegation eliminates the middleman.)

It would be much better if most interrupts went directly to S-mode:

```
S-mode kernel running
      │  (e.g., page fault or external interrupt)
      ▼
CPU stays in S-mode (or enters S-mode from U-mode)
Jumps to stvec
      │
      ▼
S-mode trap handler runs  ✓
```

### The delegation registers

RISC-V provides two CSRs for this:

| CSR | Purpose |
|-----|---------|
| `medeleg` | Machine Exception Delegation — which exceptions bypass M-mode and go to S-mode |
| `mideleg` | Machine Interrupt Delegation — which interrupts bypass M-mode and go to S-mode |

Each bit position corresponds to a trap cause code. Setting the bit
means "this trap goes to S-mode instead of M-mode."

But first — how does the CPU encode trap causes? The `scause`/`mcause`
register uses **bit 63** (the top bit on RV64) to distinguish interrupts
from exceptions:

| Bit 63 | Meaning | Lower bits |
|--------|---------|------------|
| 0 | Exception (synchronous — caused by the current instruction) | Exception code (0–15) |
| 1 | Interrupt (asynchronous — caused by external hardware) | Interrupt code (0–11) |

For example:
- `scause = 0x0000000000000002` → bit 63 = 0 → exception, code 2 →
  illegal instruction
- `scause = 0x8000000000000007` → bit 63 = 1 → interrupt, code 7 →
  machine timer

One register, one top bit — tells you everything about what happened.
The trap handler reads `scause` and branches on this. The delegation
registers (`medeleg` for exceptions, `mideleg` for interrupts) mirror
this split — each one controls the cause codes for its category.

### Exception cause codes (`medeleg`)

The RISC-V spec defines these exception codes (the `scause`/`mcause`
value when the top bit is 0):

```
Bit   Exception                        Delegate?
───   ─────────────────────────────    ──────────
 0    Instruction address misaligned   Yes — kernel handles this
 1    Instruction access fault         Yes
 2    Illegal instruction              Yes — kernel prints diagnostics
 3    Breakpoint                       Yes
 4    Load address misaligned          Yes
 5    Load access fault                Yes
 6    Store/AMO address misaligned     Yes
 7    Store/AMO access fault           Yes
 8    Environment call from U-mode     Yes — this is how syscalls work
 9    Environment call from S-mode     No  — goes to M-mode (SBI call)
10    (reserved)                        —
11    Environment call from M-mode     No  — stays in M-mode
12    Instruction page fault           Yes — kernel handles page faults
13    Load page fault                  Yes
14    (reserved)                        —
15    Store/AMO page fault             Yes
```

Exception 9 ("ecall from S-mode") is special: this is how the kernel
calls firmware services (the SBI — Supervisor Binary Interface). If the
kernel in S-mode executes `ecall`, it's asking M-mode firmware for help
(like setting a timer). This should NOT be delegated — it must reach
M-mode. With `-bios none`, we have no SBI firmware, so this doesn't
matter yet. But it's good practice to leave it undelegated.

> **What is the SBI?**
>
> The SBI (Supervisor Binary Interface) is like a "BIOS" for RISC-V.
> It's a standardized API that M-mode firmware provides to the S-mode
> kernel. The kernel calls SBI functions via `ecall` to do things that
> require M-mode privilege:
>
> | SBI function | What it does |
> |-------------|-------------|
> | `sbi_set_timer` | Set the next timer interrupt |
> | `sbi_console_putchar` | Print a character (early boot) |
> | `sbi_shutdown` | Power off the machine |
>
> The calling convention is pure software — `ecall` takes no arguments
> itself, everything is passed in registers:
>
> | Register | Role |
> |----------|------|
> | `a7` | Extension ID (which group of functions) |
> | `a6` | Function ID (which function within the group) |
> | `a0`–`a5` | Arguments |
> | `a0`, `a1` | Return values (error code, result) |
>
> For example, calling `sbi_set_timer`:
>
> ```asm
> li    a7, 0x54494D45    # extension ID for Timer ("TIME")
> li    a6, 0             # function ID: set_timer
> mv    a0, t0            # argument: the deadline value
> ecall                   # trap to M-mode → firmware handles it
> # a0 = error code on return
> ```
>
> The CPU just sees `ecall` → exception 9 → trap to M-mode. The
> firmware reads `a7`/`a6`, dispatches the request, and returns via
> `mret`. Same pattern as Linux syscalls — `ecall` is the "knock on the
> door," registers carry the message.
>
> Real RISC-V boards run OpenSBI firmware in M-mode. bobchouOS runs
> with QEMU `-bios none` (no firmware), so we handle timers ourselves
> in M-mode (Round 2-3). You won't see SBI calls in our code.

### Interrupt cause codes (`mideleg`)

Interrupt codes (the `scause`/`mcause` value when the top bit is 1):

```
Bit   Interrupt                         Delegate?
───   ──────────────────────────────    ──────────
 0    (reserved)                         —
 1    Supervisor software interrupt     Yes — we'll use this for timer
 2    (reserved)                         —
 3    Machine software interrupt        No  — stays in M-mode (hardwired)
 4    (reserved)                         —
 5    Supervisor timer interrupt        Yes — timer tick
 6    (reserved)                         —
 7    Machine timer interrupt           No  — stays in M-mode (hardwired)
 8    (reserved)                         —
 9    Supervisor external interrupt     Yes — UART, disk, etc.
10    (reserved)                         —
11    Machine external interrupt        No  — stays in M-mode (hardwired)
```

The pattern: M-mode interrupts (bits 3, 7, 11) are never delegated —
they always go to M-mode. Everything else can be delegated to S-mode.

### What xv6 does

xv6 takes the simple approach and delegates everything it can:

```c
w_medeleg(0xffff);    // Delegate all exception types 0-15
w_mideleg(0xffff);    // Delegate all interrupt types 0-15
```

This sets bits 0-15 in both registers. But some bits are **hardwired to
zero** by the spec and can't actually be set:

- In `mideleg`: bits 3, 7, 11 (M-mode software/timer/external
  interrupts) — M-mode interrupts can never be delegated
- In `medeleg`: bits 9 and 11 (ecall from S-mode and M-mode) — these
  must always reach M-mode

So `0xffff` is a safe "delegate everything possible" value — the
hardware ignores the bits that can't be delegated.

### What bobchouOS will do

We'll follow xv6's approach:

```asm
    li    t0, 0xffff
    csrw  medeleg, t0
    csrw  mideleg, t0
```

This means: after the switch, when a trap occurs in S-mode (or U-mode,
once we have that), it goes to the S-mode trap handler at `stvec` —
not M-mode. We'll set up `stvec` in Round 2-2.

### How delegation changes the trap flow

Before delegation (default):

```
Any trap → M-mode (mtvec)
```

After delegation with 0xffff:

```
Exception/interrupt from U-mode or S-mode:
  ├── Delegated type → S-mode (stvec)
  └── Not delegated  → M-mode (mtvec)

Exception/interrupt from M-mode:
  └── Always → M-mode (mtvec)   (delegation doesn't apply)
```

In practice with `0xffff`:
- Almost everything goes to S-mode (the kernel handles it)
- Machine timer interrupt (bit 7) stays in M-mode (we'll set up a tiny
  M-mode handler in Round 2-3)
- `ecall` from S-mode (exception 9) goes to M-mode (SBI calls)

---

## Part 5: Physical Memory Protection (PMP)

### The access problem

There's a subtlety that's easy to miss: when we switch from M-mode to
S-mode, S-mode might not be able to access any memory at all.

M-mode can always access all physical memory — it's the highest
privilege. But S-mode's access to physical memory is governed by **PMP**
(Physical Memory Protection), a set of M-mode CSRs that define which
address ranges lower privilege levels can access.

If PMP is not configured, the default on most RISC-V implementations is:
**S-mode and U-mode have no access to any physical memory.** This means
that the moment `mret` switches to S-mode, the very first instruction
fetch would fail — the CPU can't read the instruction at `kmain`'s
address because PMP denies it.

> **Why does PMP exist?**
>
> PMP is a coarse-grained protection mechanism that works even before
> page tables are enabled. Think of it as a firewall between privilege
> levels and physical memory. Use cases:
>
> | Scenario | How PMP helps |
> |----------|--------------|
> | M-mode firmware protects its own memory | Set PMP to deny S/U access to firmware region |
> | Kernel hasn't enabled page tables yet | PMP grants access to RAM so S-mode can run |
> | Trusted Execution Environment (TEE) | PMP isolates secure memory from the normal OS |
>
> PMP and page tables form **two layers of protection**. PMP is the
> outer wall (M-mode firmware protects itself from the OS), page tables
> are the inner walls (OS protects processes from each other). Both must
> allow an access for it to succeed:
>
> ```
> Memory access check:
>
>   PMP allows it?
>       │
>       ├── No  → access fault (even the kernel can't touch it)
>       │
>       └── Yes → Page table allows it?
>                     │
>                     ├── No  → page fault (kernel handles it)
>                     │
>                     └── Yes → access succeeds
> ```
>
> **How real systems use PMP:** On real RISC-V boards, the OS kernel
> doesn't configure PMP — the firmware (OpenSBI) does it before handing
> off to the kernel. OpenSBI typically creates one permissive entry
> covering all memory (same as what we'll do), plus a locked entry
> (L=1) protecting its own memory region. This happens before any OS
> loads, so it's the same regardless of which kernel boots — Linux,
> FreeBSD, or anything else. By the time the kernel starts, PMP is
> already set up and the kernel never touches it. Page tables become
> the primary protection mechanism from that point.

### PMP registers

RISC-V supports up to **16 PMP entries** (numbered 0–15). Each entry
has its own address register and a configuration byte:

- **`pmpaddr0`–`pmpaddr15`** — 16 address registers, one per entry
- **`pmpcfg0`, `pmpcfg2`** — 2 configuration registers on RV64, each
  packing 8 entries' config bytes (8 bits per entry × 8 = 64 bits)

| Register | Entries |
|----------|---------|
| `pmpcfg0` | 0–7 (bits 7:0 = entry 0, bits 15:8 = entry 1, ...) |
| `pmpcfg2` | 8–15 |

> **Why `pmpcfg2` and not `pmpcfg1`?** On RV32, each register is only
> 32 bits, so you need 4 registers: `pmpcfg0`–`pmpcfg3`. On RV64, each
> register is 64 bits, so `pmpcfg0` covers entries 0–7 and `pmpcfg2`
> covers 8–15. The odd-numbered registers (`pmpcfg1`, `pmpcfg3`) don't
> exist on RV64. The spec skips them to keep the numbering consistent
> — entry 8 is always in `pmpcfg2`, regardless of platform.

We only need entry 0: "allow S-mode to access all memory."

### Addressing modes

The `pmpaddr` register holds the physical address **shifted right by 2**
(because the bottom 2 bits are always 0 for aligned addresses — so the
minimum PMP granularity is 4 bytes). For RV64, each PMP address register
stores bits [55:2] of a 56-bit physical address, giving **54 meaningful
bits**. The top 10 bits of the 64-bit register are hardwired to 0. RV64
has 56-bit physical addresses — a hard limit regardless of paging mode
(Sv39, Sv48, and Sv57 all share the same 56-bit physical address width).

The `A` field (bits 4:3) in the configuration byte selects how the
address register is interpreted:

| A value | Mode | How the region is defined |
|---------|------|--------------------------|
| `00` | OFF | Entry disabled |
| `01` | TOR | Top Of Range — region is `[pmpaddr(i-1), pmpaddr(i))` |
| `10` | NA4 | Naturally Aligned 4-byte region (exactly 4 bytes at the address) |
| `11` | NAPOT | Naturally Aligned Power Of Two — base + size encoded in one register |

**TOR** (Top Of Range) defines a region as `[pmpaddr(i-1), pmpaddr(i))`.
For entry 0, the lower bound is implicitly 0. This is what xv6 and we
use — explained below.

**NAPOT** is a clever encoding that packs both base and size into one
register. The format is:

```
pmpaddr = [base >> 2] 0 [1 × G]
           ─────────  ─  ──────
           where      │  how big: 2^(G+3) bytes
                      separator
```

The hardware finds the lowest `0` bit — everything above it is the base
address, the trailing `1`s below it encode the size. The `+3` in the
formula accounts for: 2 bits from the >>2 shift, plus 1 bit for the
separator. For example, a 64KB region at `0x80000000`:
base >> 2 = `0x20000000`, G = 13 trailing 1s → 2^(13+3) = 64KB, so
`pmpaddr = 0x20001FFF` (base bits, then `0`, then 13 `1`s). The catch:
the base must be **naturally aligned** to the region size — that's the
"NA" in NAPOT. A 64KB region can only start at multiples of 64KB. If
the base isn't aligned, its low bits collide with the NAPOT encoding
and you get a wrong address. TOR doesn't have this restriction.

**NA4** covers exactly 4 bytes at the given address — the minimum
granularity that NAPOT can't express (NAPOT's minimum is 8 bytes).

### What xv6 (and we) use: TOR

For "allow everything," xv6 uses **TOR mode** with `pmpaddr0` set to
the maximum value:

```
pmpaddr0 = 0x3F_FFFF_FFFF_FFFF   (all 54 implemented bits = 1)
```

With TOR, the region is `0 ≤ address < (pmpaddr0 << 2)` (recall from
above that `pmpaddr` always stores the physical address >> 2). So the
effective upper bound is `0x3F_FFFF_FFFF_FFFF << 2` =
`0xFF_FFFF_FFFF_FFFC` — technically missing the last 4 bytes of the
56-bit space. In practice this doesn't matter: nothing is mapped at the
very top of the address space.

The configuration byte goes in `pmpcfg0` (bits 7:0, for entry 0):

```
pmpcfg0 entry 0 (8 bits):

  Bits  Field    Value   Meaning
  ────  ─────    ─────   ───────
  7     L        0       Not locked (M-mode can change it later)
  6:5   (reserved) 0
  4:3   A        01      TOR (Top Of Range) addressing mode
  2     X        1       Execute permission
  1     W        1       Write permission
  0     R        1       Read permission
```

So the configuration byte is: `0b_0_00_01_111` = `0x0F`.

```
  bit:  7    6  5    4  3    2    1    0
       ┌────┬─────┬─────┬────┬────┬────┐
       │ 0  │ 0  0│ 0  1│  1 │  1 │  1 │  = 0x0F
       └────┴─────┴─────┴────┴────┴────┘
         L   (res)   A     X    W    R
               0    TOR
```

In assembly:

```asm
    # Allow S-mode to access all physical memory
    li    t0, 0x3fffffffffffff
    csrw  pmpaddr0, t0

    li    t0, 0x0f
    csrw  pmpcfg0, t0
```

This is exactly what xv6 does. After this, S-mode can read, write, and
execute anywhere in physical memory. In a real system, you might restrict
this (e.g., protect firmware memory), but for a learning OS running on
QEMU, "allow everything" is the right choice.

> **Lock bit (L):** If you set L = 1, the PMP entry becomes permanent —
> even M-mode can't change it until the next reset. This is how firmware
> protects its own memory from a compromised kernel. We leave L = 0
> because we don't need that protection.

---

## Part 6: Disabling Paging

One more thing before `mret`: we must ensure virtual memory (paging) is
disabled. Paging is controlled by the `satp` (Supervisor Address
Translation and Protection) CSR:

```
satp register (64-bit):

  Bits    Field    Description
  ─────   ─────    ───────────
  63:60   MODE     Translation mode: 0 = bare (no paging), 8 = Sv39, 9 = Sv48
  59:44   ASID     Address Space Identifier (for TLB tagging)
  43:0    PPN      Physical Page Number of root page table
```

When MODE = 0, paging is off — the CPU uses physical addresses directly.
This is what we want. We won't enable paging until Phase 4 when we set
up page tables.

```asm
    csrw  satp, zero      # MODE = 0, no paging
```

Writing zero sets all fields to 0, which means MODE = 0 (bare/no
translation).

> **Do we need a fence?** On real hardware, when you change `satp`, the
> CPU might still be using old translations cached in the TLB
> (Translation Lookaside Buffer). You'd normally issue an `sfence.vma`
> instruction to flush the TLB. Since we're going from "no paging" to
> "still no paging," there's nothing to flush. But it's good to know
> about — we'll need `sfence.vma` in Phase 4.

---

## Part 7: Putting It All Together — The M→S Recipe

Here's the complete sequence of operations for the M-to-S switch, and
why each one is needed:

```
Step   Operation                        Why
────   ─────────────────────────────    ─────────────────────────────────
 1     Set mstatus.MPP = Supervisor     Tell mret to switch to S-mode
 2     Write kmain address to mepc      Tell mret where to jump
 3     Write 0 to satp                  Disable paging (no page tables yet)
 4     Delegate traps (medeleg/mideleg) Let S-mode handle traps directly
 5     Configure PMP                    Grant S-mode access to all memory
 6     Execute mret                     CPU reads MPP + mepc → S-mode + kmain
```

After step 6, the CPU is in S-mode at the first instruction of `kmain`.
It can access all physical memory (PMP), there's no address translation
(satp = 0), and traps are routed to S-mode handlers (delegation).

### What happens inside the CPU during `mret`

Let's trace through the microarchitectural steps when `mret` executes:

```
mret instruction executes:

  1. Read mstatus.MPP → value is 01 (Supervisor)
  2. Set the CPU's privilege level to 01 (S-mode)
  3. Set mstatus.MIE ← mstatus.MPIE  (restore interrupt state)
  4. Set mstatus.MPIE ← 1
  5. Set mstatus.MPP ← 00 (User)     (clear MPP after use)
  6. Read mepc → address of kmain
  7. Set PC ← mepc
  8. Begin fetching instructions at the new PC
```

Step 5 is important: after `mret`, MPP is cleared to `00` (User), not
left at `01` (Supervisor). This prevents a second `mret` from staying
in S-mode — a security measure to ensure `mret` always drops privilege
unless MPP is explicitly set again by a trap.

### The flow diagram

```
entry.S (M-mode)                             kmain() (S-mode)
─────────────────                            ──────────────────
_start:                                      void kmain(void) {
  park harts                                     uart_init();
  zero BSS                                       kprintf("boot...\n");
  setup stack                                    ...
  ─── M→S setup ───                          }
  set MPP = Supervisor
  write mepc = kmain
  write satp = 0
  delegate traps
  configure PMP
  execute mret ─────────────────────────────→ (arrives here in S-mode)
```

---

## Part 8: How xv6 Does It

Let's look at xv6's implementation. xv6 splits the boot into two stages:
`entry.S` (pure assembly, minimal) and `start.c` (C code, does the
mode switch). Here's `start.c` with annotations:

```c
// kernel/start.c — xv6 M-mode initialization

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"

void main();      // forward declaration
void timerinit(); // timer setup (Round 2-3 topic)

// entry.S jumps here in M-mode on each CPU's stack.
void
start()
{
    // ---- Step 1: Set MPP to Supervisor ----
    unsigned long x = r_mstatus();   // read mstatus
    x &= ~MSTATUS_MPP_MASK;         // clear MPP bits (12:11)
    x |= MSTATUS_MPP_S;             // set MPP = 01 (Supervisor)
    w_mstatus(x);                    // write mstatus back

    // ---- Step 2: Set mepc to main ----
    w_mepc((uint64)main);

    // ---- Step 3: Disable paging ----
    w_satp(0);

    // ---- Step 4: Delegate traps ----
    w_medeleg(0xffff);
    w_mideleg(0xffff);

    // ---- Step 4b: Enable S-mode interrupt types ----
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    // ---- Step 5: Configure PMP ----
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    // ---- Timer init (Round 2-3 topic) ----
    timerinit();

    // ---- Store hartid in tp register ----
    int id = r_mhartid();
    w_tp(id);

    // ---- Step 6: Switch to S-mode ----
    asm volatile("mret");
}
```

Notice the CSR access functions like `r_mstatus()` and `w_mstatus()`.
These are defined in xv6's `riscv.h` as inline assembly wrappers:

```c
// riscv.h — CSR access helpers (excerpt)

static inline uint64
r_mstatus()
{
    uint64 x;
    asm volatile("csrr %0, mstatus" : "=r" (x));
    return x;
}

static inline void
w_mstatus(uint64 x)
{
    asm volatile("csrw mstatus, %0" : : "r" (x));
}
```

The `%0` is a placeholder for the C variable. `"=r"(x)` means "put the
result in any general register, then store it in `x`." `"r"(x)` means
"put `x` in any general register and use it as input." The compiler
handles register allocation — you don't choose which register.

> **Step 4b: `sie` register.** xv6 also enables specific interrupt types
> in the `sie` (Supervisor Interrupt Enable) register. This register has
> individual enable bits for each S-mode interrupt source:
>
> | Bit | Name | Interrupt |
> |-----|------|-----------|
> | 1 | SSIE | Supervisor Software Interrupt Enable |
> | 5 | STIE | Supervisor Timer Interrupt Enable |
> | 9 | SEIE | Supervisor External Interrupt Enable |
>
> Setting these bits means "I want to receive these types of interrupts
> in S-mode." But interrupts still won't fire until the global enable
> (`sstatus.SIE`) is also set. We'll handle all of this in Round 2-3.

### Two things xv6 does that we'll defer

**Timer initialization:** xv6 calls `timerinit()` which programs the
CLINT hardware to generate periodic timer interrupts. We'll add this in
Round 2-3. For now, our kernel just halts — no timer needed.

**Hart ID in `tp`:** xv6 stores each hart's ID in the `tp` (thread
pointer) register using `w_tp(id)`. This is because after switching to
S-mode, the hart can no longer read `mhartid` (it's an M-mode CSR).
Storing the ID in `tp` allows S-mode code to identify which hart it's
running on. We only use hart 0 for now, so we'll skip this. In a
multi-core Phase, we'd add it.

---

## Part 9: How bobchouOS Will Do It

### Assembly vs C

xv6 does the mode switch in C (`start.c`), which requires the inline
assembly wrappers in `riscv.h`. We'll do it directly in `entry.S` — pure
assembly. Why?

| Approach | Pros | Cons |
|----------|------|------|
| C with inline asm (xv6) | Easier to read for C programmers, can use constants/macros | Requires a C stack and calling convention, one more file |
| Pure assembly (bobchouOS) | No C dependencies, runs before stack is needed, keeps all early boot in one file | More assembly to write |

Both are perfectly valid. We choose pure assembly because our entry.S
already does the early boot work, and adding 15 more instructions there
is simpler than creating a separate `start.c` file. The mode switch
is conceptually part of "setting up the world before C runs."

Our stack setup (`la sp, stack_top`) is already in entry.S and must
happen before any C call. The question is: do we switch modes before or
after setting up the stack? The answer is **before** — because `mret`
is what jumps to `kmain`, so the stack must be ready, but we never
actually call a C function in M-mode. The order will be:

```
_start:
  1. Park harts
  2. Zero BSS
  3. Set up stack
  4. Configure M→S switch (still M-mode, using assembly)
  5. mret → arrives at kmain in S-mode, stack already set up
```

### The `riscv.h` header

Even though we do the switch in assembly, we still want CSR access
macros for C code. Starting from Round 2-2, the trap handler (C code)
needs to read `scause`, `sepc`, `stval`, etc. So we'll create
`kernel/include/riscv.h` with helper macros now:

```c
// riscv.h — RISC-V CSR access helpers

#ifndef RISCV_H
#define RISCV_H

#include "types.h"

// ---- CSR read/write macros ----
//
// These use GCC inline assembly to generate csrr/csrw instructions.
// The macro approach avoids writing a separate function for each CSR.

#define csrr(csr)                                       \
    ({                                                  \
        uint64 __val;                                   \
        asm volatile("csrr %0, " #csr : "=r"(__val));   \
        __val;                                          \
    })

#define csrw(csr, val)                                  \
    ({                                                  \
        asm volatile("csrw " #csr ", %0" : : "r"(val)); \
    })

// ---- mstatus bits ----
#define MSTATUS_MPP_MASK    (3UL << 11)
#define MSTATUS_MPP_M       (3UL << 11)
#define MSTATUS_MPP_S       (1UL << 11)
#define MSTATUS_MPP_U       (0UL << 11)
#define MSTATUS_MIE         (1UL << 3)
#define MSTATUS_MPIE        (1UL << 7)

// ---- sstatus bits ----
#define SSTATUS_SPP         (1UL << 8)
#define SSTATUS_SPIE        (1UL << 5)
#define SSTATUS_SIE         (1UL << 1)

#endif // RISCV_H
```

The `csrr`/`csrw` macros use a GCC extension called **statement
expressions** (`({ ... })`) that lets a block of code produce a value.
The `#csr` uses the C preprocessor's **stringification** operator to
turn the macro argument into a string. So `csrr(sstatus)` expands to:

```c
({
    uint64 __val;
    asm volatile("csrr %0, sstatus" : "=r"(__val));
    __val;
})
```

> **xv6's approach vs ours:** xv6 writes a separate `r_sstatus()`,
> `w_sstatus()`, `r_scause()`, `w_scause()` function for every CSR —
> about 40 functions in `riscv.h`. Our macro approach covers all CSRs
> with just two macros. The trade-off: xv6's approach is more explicit
> (you can see every CSR that's used), ours is more concise. Both
> generate identical machine code.

### Verifying the switch

How do we know the switch worked? We can read `sstatus` in `kmain()` and
print the SPP bit. But the real proof is simpler: if `kmain()` runs at
all, the switch succeeded — because `mret` with MPP = Supervisor
is the instruction that jumps to `kmain`. If anything went wrong (bad
MPP, bad `mepc`, PMP not configured), the CPU would fault before
reaching `kmain`.

Still, for educational purposes, we'll add a simple check:

```c
void kmain(void) {
    uart_init();
    kprintf("\nbobchouOS is booting...\n");
    kprintf("running in S-mode\n");

    // Read sstatus — if we can read it, we're in S-mode (or M-mode).
    // If we were in U-mode, this would cause an exception.
    uint64 sstatus_val = csrr(sstatus);
    kprintf("sstatus = %p\n", (void *)sstatus_val);
    ...
}
```

---

## Part 10: Comparing bobchouOS and xv6

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Where the switch happens | `start.c` (C code) | `entry.S` (assembly) |
| CSR access | Individual inline functions (`r_mstatus()`, etc.) | Generic macros (`csrr(mstatus)`, etc.) |
| Delegation | `0xffff` for both | `0xffff` for both (same) |
| PMP | One entry, all memory, RWX | Same |
| `satp` | Set to 0 | Same |
| Timer init | Done in `start.c` | Deferred to Round 2-3 |
| Hart ID in `tp` | Yes (for multi-core) | Skipped (single hart for now) |
| SIE enable bits | Set in `start()` | Deferred to Round 2-3 |

The core logic is identical. The differences are packaging (C vs
assembly, all-at-once vs spread across rounds).

---

## What's Next

After you read this lecture, we'll:

1. **Create the skeleton** — modified `entry.S` with TODO markers for
   the M→S switch, plus a new `kernel/include/riscv.h` with CSR macros
2. **You implement the TODOs** — about 15 lines of assembly in `entry.S`
3. **Verify** — `make run` should show "bobchouOS is booting" just like
   before, but now running in S-mode (confirmed by sstatus print)

Then we move to Round 2-2: setting up the trap vector and writing a
trap handler — the code that runs when interrupts and exceptions happen.

---

## Quick Reference

### M→S switch recipe (assembly)

| Step | Instruction(s) | Purpose |
|------|----------------|---------|
| Set MPP | `csrr` / mask / `csrw mstatus` | Tell `mret` to switch to S-mode |
| Set mepc | `la t0, kmain; csrw mepc, t0` | Tell `mret` where to jump |
| Disable paging | `csrw satp, zero` | No page tables yet |
| Delegate exceptions | `li t0, 0xffff; csrw medeleg, t0` | S-mode handles exceptions |
| Delegate interrupts | `csrw mideleg, t0` | S-mode handles interrupts |
| PMP address | `li t0, 0x3fffffffffffff; csrw pmpaddr0, t0` | Cover all memory |
| PMP config | `li t0, 0xf; csrw pmpcfg0, t0` | TOR + RWX |
| Switch | `mret` | Jump to mepc in S-mode |

### Key CSRs for M→S switch

| CSR | Bits | Purpose |
|-----|------|---------|
| `mstatus` | 12:11 (MPP) | Target mode for `mret` |
| `mstatus` | 7 (MPIE) | Interrupt state restored by `mret` |
| `mstatus` | 3 (MIE) | Global M-mode interrupt enable |
| `mepc` | 64-bit addr | Jump target for `mret` |
| `medeleg` | 16 bits | Exception delegation to S-mode |
| `mideleg` | 16 bits | Interrupt delegation to S-mode |
| `satp` | 63:60 (MODE) | Paging mode (0 = disabled) |
| `pmpaddr0` | 54-bit addr | PMP region address (shifted >> 2) |
| `pmpcfg0` | 7:0 | PMP entry 0 config (L/A/XWR) |

### Exception cause codes (for reference)

| Code | Exception |
|------|-----------|
| 0 | Instruction address misaligned |
| 1 | Instruction access fault |
| 2 | Illegal instruction |
| 3 | Breakpoint |
| 4 | Load address misaligned |
| 5 | Load access fault |
| 6 | Store/AMO address misaligned |
| 7 | Store/AMO access fault |
| 8 | Environment call from U-mode |
| 9 | Environment call from S-mode |
| 12 | Instruction page fault |
| 13 | Load page fault |
| 15 | Store/AMO page fault |

### Interrupt cause codes (for reference)

| Code | Interrupt |
|------|-----------|
| 1 | Supervisor software interrupt |
| 3 | Machine software interrupt |
| 5 | Supervisor timer interrupt |
| 7 | Machine timer interrupt |
| 9 | Supervisor external interrupt |
| 11 | Machine external interrupt |

### CSR access macros (riscv.h)

```c
csrr(csr)          // Read: uint64 val = csrr(sstatus);
csrw(csr, val)     // Write: csrw(satp, 0);
```

### RISC-V instructions introduced in this lecture

| Instruction | Meaning |
|------------|---------|
| `csrr rd, csr` | Read CSR into register |
| `csrw csr, rs` | Write register into CSR |
| `mret` | Return from M-mode trap: jump to `mepc`, switch to mode in MPP |
| `sret` | Return from S-mode trap: jump to `sepc`, switch to mode in SPP |
| `sfence.vma` | Flush TLB (not used yet, mentioned as preview for Phase 4) |
