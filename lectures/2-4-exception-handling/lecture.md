# Lecture 2-4: Exception Handling

> **Where we are**
>
> The kernel boots into S-mode, has a working trap infrastructure
> (`kernel_vec` + `kernel_trap()`), and receives periodic timer interrupts
> via the CLINT/SSIP forwarding chain. Timer ticks print once per second.
> The interrupt side of our trap handler is solid.
>
> But the exception side is a one-liner:
>
> ```c
> panic("kernel_trap: exception scause=%p sepc=%p stval=%p", ...);
> ```
>
> Every exception — whether it's a page fault, an illegal instruction,
> a breakpoint, or a system call — hits the same `panic()` and the
> kernel dies. That was fine for getting timer interrupts working, but
> it's a dead end. We can't even test exception handling because every
> exception is fatal.
>
> This lecture fixes that. We'll decode `scause` into human-readable
> exception names, use `stval` to provide context-specific diagnostics,
> distinguish recoverable exceptions from truly fatal ones, and handle
> the `ecall` instruction properly (including advancing `sepc` past it).
> By the end of Round 2-4, our trap handler will be a real diagnostic
> tool — when something goes wrong, it tells you *what* happened and
> *where*, not just three hex numbers.
>
> By the end of this lecture, you will understand:
>
> - All 16 RISC-V exception cause codes and what triggers each one
> - How `stval` provides different information for different exceptions
> - The difference between synchronous exceptions and asynchronous
>   interrupts (and why it matters for `sepc`)
> - Why `sepc` points to the *faulting instruction* for exceptions but
>   the *next instruction* for ecalls needs manual adjustment
> - How to build a human-readable exception name table
> - Which exceptions are recoverable in a kernel context vs. which are
>   always fatal
> - The `ecall` instruction: what it does, how the hardware handles it,
>   and why `sepc` must be advanced by 4
> - The `ebreak` instruction: breakpoints for debuggers
> - A preview of page-fault exceptions (COW fork, lazy allocation,
>   demand paging) — the most powerful use of exceptions in real OSes
> - How xv6 handles kernel exceptions (spoiler: it panics, same as us
>   — but user-space exceptions are where the interesting logic lives)
> - How bobchouOS will implement exception dispatch
>
> **xv6 book coverage:** This lecture absorbs the exception-handling side
> of Chapter 4, section 4.5 (Traps from kernel space) — we covered the
> interrupt side in Lecture 2-3. It also absorbs section 4.6 (Page-fault
> exceptions) as a preview — we won't implement page fault handling until
> Phase 4 (page tables), but understanding *why* page faults exist gives
> you context for the exception codes you'll see. Section 4.7 (Real
> world) is incorporated where relevant.

---

## Part 1: Exceptions vs. Interrupts — A Closer Look

### Quick review

In Lecture 2-2, we introduced the three flavors of traps:

| Flavor | Trigger | Asynchronous? | Example |
|--------|---------|:-------------:|---------|
| **Interrupt** | External hardware event | Yes | Timer tick, UART data ready |
| **Exception** | Something the instruction itself caused | No | Illegal opcode, page fault |
| **Ecall** | Explicit `ecall` instruction | No | System call |

We've been handling interrupts (timer ticks) since Round 2-3. Now it's
time for exceptions and ecalls.

### Why the distinction matters

The key difference is **when** the event happens relative to the
instruction stream:

**Interrupts are asynchronous.** A timer tick can fire between *any*
two instructions. `sepc` points to the instruction that was about to
execute when the interrupt arrived — it hasn't run yet. When the
handler does `sret`, the CPU executes that instruction normally. No
instruction needs to be re-executed or skipped.

**Exceptions are synchronous.** They are caused *by* a specific
instruction that the CPU was trying to execute. A page fault happens
because *this particular* `ld` instruction tried to access an unmapped
address. An illegal instruction exception happens because *this
particular* instruction has an invalid encoding or requires higher
privilege. The faulting instruction did NOT complete. `sepc` points to
the faulting instruction itself.

This has a critical consequence for the handler:

```
After an interrupt:
  sepc → next instruction (already saved by hardware)
  sret → resumes at next instruction → correct

After an exception:
  sepc → faulting instruction (not yet executed)
  sret → re-executes the faulting instruction

  If the handler FIXED the cause (e.g., mapped the missing page):
    sret → re-execute → succeeds now → correct

  If the handler did NOT fix the cause:
    sret → re-execute → same exception → infinite loop!
```

This is why page fault handlers work: the handler maps the missing
page, returns, and the CPU re-executes the `ld` instruction — which
succeeds this time because the page is now mapped. The instruction
retires transparently. The user program never knows anything happened.

But for exceptions you *can't* fix (like an illegal instruction in
kernel code — the instruction is wrong, you can't make it "un-wrong"),
the only options are:

1. **Skip it** — advance `sepc` past the faulting instruction and
   return. The faulting instruction is silently skipped. This is
   almost never correct for unexpected exceptions.
2. **Kill the offender** — if it's a user process, terminate it. If
   it's the kernel, panic.

> **How many bytes to skip?** RISC-V instructions are either 4 bytes
> (base ISA) or 2 bytes (compressed "C" extension). QEMU's `virt`
> machine uses the base ISA with the C extension, so both lengths
> appear in practice. You need to detect which one you're looking at.
>
> RISC-V instructions are stored as one or more 16-bit "parcels" in
> little-endian order. The length is encoded in bits 1:0 of the
> **first parcel** (the 2 bytes at the instruction's address):
>
> ```
> 2-byte compressed instruction at 0x1000:
>   Memory: [parcel0 at 0x1000]
>   parcel0 bits 1:0 = 0b00, 0b01, or 0b10
>   → "I am the whole instruction (2 bytes)"
>
> 4-byte base instruction at 0x1000:
>   Memory: [parcel0 at 0x1000] [parcel1 at 0x1002]
>   parcel0 bits 1:0 = 0b11
>   → "I am 4 bytes long — parcel1 is my other half"
>   (parcel1's low bits are just instruction payload, not length info)
> ```
>
> So the detection is a single 16-bit read and a 2-bit check:
>
> ```c
> uint16 inst = *(uint16 *)sepc_val;
> int inst_len = (inst & 0x3) == 0x3 ? 4 : 2;
> csrw(sepc, sepc_val + inst_len);
> ```
>
> We'll use this approach in our handler.

### The `ecall` special case

The `ecall` instruction is classified as an exception (synchronous,
`scause` bit 63 = 0), but it behaves differently from "fault"
exceptions:

- **Faults** (page fault, illegal instruction): the instruction
  *failed*. `sepc` points to it so the CPU can retry after the handler
  fixes the problem.
- **Ecall**: the instruction *succeeded*. The whole purpose of `ecall`
  is to trap into the kernel. It did exactly what it was supposed to.
  You don't want to re-execute it — that would trap again in an
  infinite loop.

So the handler must **manually advance `sepc`** past the `ecall`
before `sret`. The `ecall` instruction is always 4 bytes — there is no
compressed `c.ecall` (the assembler rejects it). We can hardcode `+4`:

```c
if (scause == EXC_ECALL_U || scause == EXC_ECALL_S) {
    // ecall succeeded — advance past it (always 4 bytes, no c.ecall)
    csrw(sepc, csrr(sepc) + 4);
    // ... handle the system call / SBI request ...
}
```

Without this, `sret` jumps back to the `ecall`, which traps again,
which jumps back to the `ecall` ... forever.

> **x86 comparison: faults vs. traps**
>
> x86 makes a formal distinction in naming:
>
> - **Fault**: saved instruction pointer = faulting instruction
>   (e.g., page fault, #GP). Handler is expected to fix the issue and
>   retry.
> - **Trap**: saved instruction pointer = *next* instruction after the
>   trapping one (e.g., `int 0x80`, breakpoint `int 3`). No retry
>   needed.
> - **Abort**: unrecoverable error (e.g., double fault, machine check).
>   No meaningful saved state.
>
> On x86, the hardware distinguishes these — faults save the IP *at*
> the instruction, traps save it *after*. Software doesn't need to
> manually adjust.
>
> RISC-V doesn't make this distinction in hardware. `sepc` always
> points to the trapping instruction (whether it's a fault or an
> ecall). The software must decide whether to advance `sepc`. This is
> simpler hardware but more work for software.

---

## Part 2: The `scause` Exception Codes

### The full table

When bit 63 of `scause` is 0, the lower bits identify the exception
type. The RISC-V privilege spec defines these codes:

```
scause    Name                    Description
------    --------------------    ------------------------------------------
  0       Instruction misalign    PC not aligned to instruction boundary
  1       Instruction access      No permission to fetch from this address
  2       Illegal instruction     Invalid opcode or CSR access violation
  3       Breakpoint              ebreak instruction executed
  4       Load misalign           Load address not naturally aligned
  5       Load access fault       No permission to read from this address
  6       Store/AMO misalign      Store address not naturally aligned
  7       Store/AMO access fault  No permission to write to this address
  8       Ecall from U-mode       ecall executed in U-mode (system call)
  9       Ecall from S-mode       ecall executed in S-mode
 10       (reserved)
 11       Ecall from M-mode       ecall executed in M-mode
 12       Instruction page fault  Page table: no valid mapping for fetch
 13       Load page fault         Page table: no valid mapping for load
 14       (reserved)
 15       Store/AMO page fault    Page table: no valid mapping for store
```

The spec defines 13 exception codes (0-9, 11-13, 15), with 10 and 14
reserved. Not all appear in `scause` — codes 9 and 11 depend on
delegation (see Group 4). Let's understand each group.

### Group 1: Misalignment exceptions (codes 0, 4, 6)

These fire when a memory access isn't naturally aligned:

```
Natural alignment:
  1-byte (lb/sb)   → any address OK
  2-byte (lh/sh)   → address must be multiple of 2
  4-byte (lw/sw)   → address must be multiple of 4
  8-byte (ld/sd)   → address must be multiple of 8

Misalignment examples:
  lw t0, 0(a0)     where a0 = 0x80000001  →  cause 4 (load misalign)
  sd t0, 0(a0)     where a0 = 0x80000005  →  cause 6 (store misalign)
```

**Code 0 (instruction misalignment)** fires when the PC is not aligned
to the instruction boundary. With the C extension, instructions must be
2-byte aligned; without it, 4-byte aligned. Branching to an odd address
(`jalr` to 0x80000001) triggers cause 0. This usually means a corrupted
function pointer or a bad jump target.

**Codes 4 and 6 (load/store misalignment)** fire when a data access
isn't naturally aligned. The "AMO" in "Store/AMO" stands for Atomic
Memory Operation — the read-modify-write instructions from the RISC-V
"A" (Atomic) extension (`amoswap`, `amoadd`, etc.). They share the
store exception codes because they write to memory. We won't use
atomics until Phase 5 (locking), but that's why the codes say
"Store/AMO" instead of just "Store."

### How common are misalignment exceptions?

On many RISC-V implementations, they're actually rare. **QEMU** handles
misaligned loads/stores transparently (no trap). Only misaligned
*atomics* (`lr.w`/`sc.w`) always trap. **Real hardware** varies — some
chips handle it in hardware (slower but no trap), some always trap. The
RISC-V spec says it's implementation-defined.

When the hardware does trap, the handler can *emulate* the misaligned
access in software. For example, if `lw t0, 0(a0)` traps because
`a0 = 0x80000003`, the handler reads four individual bytes from
0x80000003-0x80000006 using `lb` (which has no alignment requirement),
assembles them in little-endian order, writes the result to `t0`,
advances `sepc`, and returns. The program continues as if the `lw`
succeeded. Linux does this for user programs.

But the emulation faithfully reads whatever bytes happen to be at those
addresses — it doesn't check whether anyone actually stored a 4-byte
value there. If the address straddles two separately-written values,
you get garbage:

```
sw 0xAABBCCDD to 0x80000000  →  bytes: DD CC BB AA at 00,01,02,03
sw 0x11223344 to 0x80000004  →  bytes: 44 33 22 11 at 04,05,06,07

lw from 0x80000003 (misaligned, straddles both words):
  reads bytes at 03,04,05,06 → AA, 44, 33, 22
  assembles: 0x223344AA  ← nobody ever stored this value
```

This is why misaligned access is almost always a bug — you're reading
bytes that span two different values, getting a meaningless result.
It's the programmer's responsibility to ensure loads and stores are
properly aligned. For now, we'll panic on any misalignment because
only kernel code is running. In Phase 6, when we have user processes,
a misaligned access from a user program will kill just that process
(like Linux sending SIGBUS) — the kernel shouldn't crash because
someone else's code has a bug.

### Group 2: Access faults (codes 1, 5, 7)

Access faults fire when the CPU detects a **physical memory protection
(PMP) violation** or the address is in a region that doesn't exist.
These are checked against physical addresses — when paging is enabled,
PMP checks happen *after* page table translation produces the physical
address.

```
Code 1: Instruction access fault
  → Tried to fetch an instruction from a PMP-protected or non-existent
    physical address.

Code 5: Load access fault
  → Tried to read from a PMP-protected or non-existent physical address.

Code 7: Store/AMO access fault
  → Tried to write to a PMP-protected or non-existent physical address.
```

> **Access faults vs. page faults — what's the difference?**
>
> This is a common source of confusion. Both result from "bad
> addresses," but they're checked at different stages:
>
> ```
> Virtual address
>       |
>       v
> Page table translation (if paging enabled)
>   - No valid mapping? → PAGE FAULT (code 12/13/15)
>   - Permission denied? → PAGE FAULT (code 12/13/15)
>       |
>       v
> Physical address
>       |
>       v
> PMP check + physical memory range check
>   - PMP denied? → ACCESS FAULT (code 1/5/7)
>   - Address doesn't exist? → ACCESS FAULT (code 1/5/7)
>       |
>       v
> Memory/device responds
> ```
>
> **Page faults** are about *virtual-to-physical translation* — the
> page table doesn't have a mapping, or the permission bits (PTE_R,
> PTE_W, PTE_X) deny the access. Page faults only happen when paging
> is enabled (`satp` is nonzero).
>
> **Access faults** are about *physical access control* — the PMP
> says you can't touch this physical address, or the address doesn't
> map to any device or RAM. Access faults happen regardless of whether
> paging is enabled.
>
> Right now, we have paging disabled (`satp = 0`). All addresses are
> physical. So we'll only see access faults (codes 1, 5, 7), never
> page faults (codes 12, 13, 15). Page faults become relevant in
> Phase 4 when we enable the Sv39 page table.
>
> | Exception type | When paging OFF | When paging ON |
> |---------------|:---------------:|:--------------:|
> | Access fault (1, 5, 7) | Yes (PMP/range) | Yes (PMP/range, after translation) |
> | Page fault (12, 13, 15) | Never | Yes (translation failure) |

### Group 3: Illegal instruction (code 2)

This is the most common exception you'll see during kernel development.
It fires when the CPU encounters an instruction it can't execute. The
causes include:

1. **Invalid opcode** — the instruction encoding doesn't match any
   defined instruction. This usually means corrupted code or a jump
   to a data region.

2. **CSR privilege violation** — trying to access a CSR that requires
   higher privilege. Example: `csrr t0, mhartid` from S-mode. The
   `mhartid` CSR has number `0xF14` — bits 9:8 = `0b11` = M-mode
   only. This is how we tested our trap handler in Round 2-2.

3. **Disabled extension** — trying to use floating-point instructions
   when `mstatus.FS = 0` (FPU disabled), or trying to use vector
   instructions when the V extension is disabled.

For illegal instruction exceptions, `stval` is supposed to contain
the faulting instruction encoding. However, this is
implementation-defined — the spec says `stval` *may* be set to the
instruction value, or it *may* be zero. QEMU sets `stval` to the instruction encoding,
which is helpful for diagnostics.

### Group 4: Ecalls (codes 8, 9, 11)

Each privilege mode gets its own ecall code:

| Mode | Code | Appears in `scause`? | When |
|------|:----:|:--------------------:|------|
| U-mode | 8 | Yes (delegated to S-mode) | User process makes a system call |
| S-mode | 9 | Yes (delegated to S-mode) | Kernel calls into firmware (SBI) |
| M-mode | 11 | No (always traps within M-mode) | M-mode self-call (rare) |

**Code 8 (ecall from U-mode)** is the most important one. This is how
user programs request kernel services — `read()`, `write()`, `fork()`,
`exit()`. The user program puts the syscall number in register `a7`,
arguments in `a0`-`a5`, and executes `ecall`. The trap handler reads
`a7` to identify which syscall and dispatches to the right function.
We'll implement this in Phase 6.

**Code 9 (ecall from S-mode)** is what SBI firmware (like OpenSBI)
expects. On real hardware with SBI, the kernel would use `ecall` to
request firmware services like `sbi_set_timer()`. Since we run with
`-bios none`, no firmware is listening.

Where does ecall from S-mode actually trap? It depends on `medeleg`
bit 9 (see the delegation blockquote that follows). With our `0xffff`, it's
delegated to S-mode, and `kernel_trap()` sees `scause = 9`. For Round
2-4, we'll handle it by printing a diagnostic and advancing `sepc`
past the ecall, but we won't process any SBI calls.

> **The delegation rules for ecalls.** The RISC-V privilege spec
> (section 3.3.1) says: *"ECALL generates a different exception for
> each originating privilege mode so that environment call exceptions
> can be selectively delegated."* Where each ecall traps depends on
> `medeleg`, just like any other exception:
>
> - **Code 8 (U-mode):** `medeleg` bit 8 is settable. We set it, so
>   U-mode ecalls trap to S-mode — the normal system call path. If
>   NOT set, they would skip S-mode and trap to M-mode directly.
> - **Code 9 (S-mode):** `medeleg` bit 9 is settable. We set it (via
>   `0xffff`), so S-mode ecalls are delegated to S-mode. If NOT set,
>   they trap to M-mode. On real hardware with OpenSBI, firmware
>   typically leaves bit 9 clear so that S-mode ecalls reach M-mode
>   where the SBI handler runs.
> - **Code 11 (M-mode):** `medeleg` bit 11 is **hardwired to 0** —
>   the spec explicitly says "medeleg[11] is read-only zero." M-mode
>   ecalls always trap within M-mode.
>
> The spec (section 3.1.8) only explicitly hardwires bit 11. The
> common claim that "bit 9 is also hardwired" appears in some teaching
> materials but is not in the spec — and QEMU confirms bit 9 is
> writable. The spec *does* say "A typical use case for Unix-like
> operating systems is to delegate to S-mode the
> environment-call-from-U-mode exception but not the others" — but
> "typical" is a recommendation, not a hardware constraint.

**Code 11 (ecall from M-mode)** will never appear in `scause`. It
traps within M-mode itself (setting `mcause`, not `scause`), and it
can't be delegated (`medeleg` bit 11 is hardwired to 0). We include
it in our exception name table for completeness, but no S-mode handler
will ever see it.

### Group 5: Breakpoint (code 3)

The `ebreak` instruction generates a breakpoint exception. This is how
debuggers work:

1. GDB saves the original instruction bytes and overwrites them with
   an `ebreak` or `c.ebreak` instruction.
2. When the CPU hits the breakpoint, it traps with `scause = 3`.
3. GDB's trap handler takes over, lets you inspect registers and
   memory.
4. When you say "continue," GDB restores the original instruction,
   single-steps past it, re-inserts the breakpoint, and resumes.

Like `ecall`, `ebreak` is an intentional trap. The handler should
advance `sepc` past the instruction if it wants to skip the breakpoint.
Or it can leave `sepc` alone to re-execute (for the GDB-style save-
and-restore workflow).

Unlike `ecall`, `ebreak` has a compressed variant: `c.ebreak` (2
bytes). And it's not just theoretical — our compiler **will** use it.
We can verify by comparing the output with and without the C extension:

```
With -march=rv64imac (our build — C extension enabled):
   0:   9002        ebreak          ← 2 bytes! assembler chose c.ebreak
   2:   00000073    ecall           ← 4 bytes (no c.ecall exists)
   6:   8082        ret             ← 2 bytes (c.ret)

With -march=rv64ima (C extension disabled):
   0:   00100073    ebreak          ← 4 bytes
   4:   00000073    ecall           ← 4 bytes
   8:   00008067    ret             ← 4 bytes (jalr zero, ra, 0)
```

Since our Makefile uses `-march=rv64imac`, the assembler automatically
compresses `ebreak` to `c.ebreak` (2 bytes). If we hardcoded
`sepc += 4`, we'd skip the instruction *after* the ebreak too — a
nasty silent bug. So we use the instruction length detection from
Part 1 (read the first parcel, check bits 1:0).

This size difference also matters for debuggers. A debugger could
always use `c.ebreak` (2 bytes fits in any instruction slot), or it
could match the original instruction's size. The exact strategy is an
implementation detail — what matters is that it saves enough bytes to
restore the original instruction later.

> **x86 comparison.** On x86, the breakpoint instruction `int 3` has a
> special single-byte encoding (`0xCC`) — the smallest possible
> instruction size on x86. Debuggers always write one byte, and the
> CPU traps immediately on decoding `0xCC` without looking at the
> remaining bytes. This single-byte design was intentional — Intel
> gave `int 3` its own dedicated opcode specifically for debugger use.
> On RISC-V, the smallest breakpoint is 2 bytes (`c.ebreak`), which
> still fits in any instruction slot since all instructions are at
> least 2 bytes with the C extension.

For bobchouOS, we'll treat `ebreak` as a non-fatal diagnostic:
print the breakpoint address, advance `sepc` by the correct length,
and continue.

### Group 6: Page faults (codes 12, 13, 15)

These fire when the page table translation fails:

| Code | Name | Trigger |
|:----:|------|---------|
| 12 | Instruction page fault | Fetching instruction from unmapped/protected virtual address |
| 13 | Load page fault | Reading from unmapped/protected virtual address |
| 15 | Store page fault | Writing to unmapped/protected virtual address |

Page faults are the most *useful* exceptions in a modern OS. Unlike
most other exceptions (which indicate bugs), page faults are expected
events that the kernel handles routinely. We'll cover this in detail
in Part 6.

For now, with paging disabled (`satp = 0`), we'll never see page
faults. Our handler will treat them as fatal (panic), with a note
that Phase 4 will make them useful.

---

## Part 3: The `stval` Register — Exception Context

### What `stval` tells you

When a trap fires, the hardware writes additional context to `stval`
(Supervisor Trap Value). What `stval` contains depends on the exception
type:

| Exception | `stval` contains | Why it's useful |
|-----------|-----------------|-----------------|
| Misalignment (0, 4, 6) | The misaligned virtual address | Shows what address caused the fault |
| Access fault (1, 5, 7) | The faulting virtual address | Shows what address was denied |
| Illegal instruction (2) | The faulting instruction encoding (or 0) | Shows what instruction was invalid |
| Breakpoint (3) | The `ebreak` instruction's address (or 0) | Same as `sepc` |
| Ecall (8, 9, 11) | 0 | No additional context needed |
| Page fault (12, 13, 15) | The faulting virtual address | The address that needs a page mapping |

The most critical case is **page faults**: `stval` tells the handler
*which address* faulted, so the handler can map the right page. Without
`stval`, the handler would have to decode the faulting instruction to
figure out what address it was trying to access — much more work.

For **illegal instruction** exceptions, `stval` can contain the
instruction word. This is implementation-defined (the spec says
`stval` "may" contain the instruction). QEMU does set it, which lets
us print the actual bad instruction. This is great for debugging:

```
exception: illegal instruction
  sepc=0x800001a4  stval=0xf14025f3
                               ^^^^
                   This is the machine-code encoding of "csrr t0, mhartid"
```

> **Decoding `stval` for illegal instructions.** The RISC-V
> instruction encoding for `csrr t0, mhartid` is:
>
> ```
> Bit layout of a CSR-read instruction (CSRRS rd, csr, zero):
>
>   [31:20]   [19:15]  [14:12]  [11:7]   [6:0]
>   csr       rs1      funct3   rd       opcode
>   0xF14     00000    010      01011    1110011
>   mhartid   zero     CSRRS    t0       SYSTEM
>
> Encoded: 1111_0001_0100_00000_010_01011_1110011
>        = 0xF14025F3
> ```
>
> You probably won't decode `stval` by hand often, but it's useful
> when debugging unexpected traps. GDB or `objdump` can disassemble
> it for you:
>
> ```bash
> echo "f3 25 40 f1" | xxd -r -p > /tmp/inst.bin
> riscv-none-elf-objdump -b binary -m riscv:rv64 -D /tmp/inst.bin
> ```

For **ecalls**, `stval` is always 0 — the trap value is meaningless
because ecalls are intentional. The syscall number is in register `a7`,
not in `stval`.

### `stval` is not guaranteed by the base spec

The RISC-V privilege spec does not guarantee `stval` for **any**
exception type. Every sentence about `stval` contents is conditional:
"if `stval` is written with a nonzero value when a breakpoint,
address-misaligned, access-fault, or page-fault exception occurs ...
then `stval` will contain the faulting virtual address." The *if* is
doing all the work — the implementation is free to write zero instead.
The spec explicitly defers the decision: "The hardware platform will
specify which exceptions must set `stval` informatively and which may
unconditionally set it to zero."

In practice, platforms do set `stval` for the exceptions where handlers
need it:

| Exception type | QEMU `stval` behavior | Real hardware |
|---------------|----------------------|---------------|
| Access/page faults | Always set to faulting address | Set in practice (platform-dependent) |
| Illegal instruction | Set to instruction encoding | Varies (may be 0) |
| Misalignment | Set to faulting address | Usually set |
| Breakpoint | Set to `ebreak` address | Varies |
| Ecall | Always 0 | Always 0 |

The practical takeaway: on QEMU `virt`, `stval` is set for all
exception types in the table above. Our handler will rely on `stval`
for access faults and page faults (where the faulting address is
essential for the handler logic), and *print* it for everything else
as a best-effort diagnostic. If we ever port to real hardware, the
access/page fault behavior should hold (every platform needs it), but
the other values would need checking against that platform's spec.

---

## Part 4: Building the Exception Dispatch

### From one panic to a proper handler

Here's what we have now:

```c
// Current code in trap.c — exception handling
} else {
    panic("kernel_trap: exception scause=%p sepc=%p stval=%p",
          scause_val, sepc_val, stval_val);
}
```

Here's what we want:

```c
} else {
    // Exception — decode scause and provide human-readable output
    const char *name = exc_name(scause_val);

    // Detect instruction length at sepc (4-byte vs 2-byte compressed)
    uint16 inst = *(uint16 *)sepc_val;
    int inst_len = (inst & 0x3) == 0x3 ? 4 : 2;

    switch (scause_val) {
    case EXC_BREAKPOINT:
        // Non-fatal: print diagnostic and continue
        kprintf("breakpoint at %p\n", sepc_val);
        csrw(sepc, sepc_val + inst_len);  // skip past ebreak/c.ebreak
        return;

    case EXC_ECALL_S:
        // Non-fatal: unhandled ecall from S-mode, print and skip
        kprintf("ecall from S-mode at %p (no SBI handler)\n", sepc_val);
        csrw(sepc, sepc_val + 4);  // ecall is always 4 bytes
        return;

    // EXC_ECALL_U: syscall from user process — Phase 6

    default:
        // Fatal: print diagnostics and panic
        panic("kernel_trap: %s"
              "  scause=%p  sepc=%p  stval=%p",
              name, scause_val, sepc_val, stval_val);
    }
}
```

### The exception name table

Rather than a big `switch` statement to convert codes to strings, a
simple lookup table is cleaner:

```c
static const char *exc_names[] = {
    [0]  = "instruction address misaligned",
    [1]  = "instruction access fault",
    [2]  = "illegal instruction",
    [3]  = "breakpoint",
    [4]  = "load address misaligned",
    [5]  = "load access fault",
    [6]  = "store/AMO address misaligned",
    [7]  = "store/AMO access fault",
    [8]  = "ecall from U-mode",
    [9]  = "ecall from S-mode",
    [11] = "ecall from M-mode",
    [12] = "instruction page fault",
    [13] = "load page fault",
    [15] = "store/AMO page fault",
};

static const char *
exc_name(uint64 scause) {
    if (scause < sizeof(exc_names) / sizeof(exc_names[0])
        && exc_names[scause])
        return exc_names[scause];
    return "unknown exception";
}
```

This uses C's designated initializer syntax (`[0] = "..."`) to fill
in each array element by index. Entries 10 and 14 (reserved codes)
are left as `NULL`, and `exc_name()` returns `"unknown exception"` for
those. The table is small (16 entries, 16 pointers = 128 bytes) and
gives us instant name lookup.

> **Designated initializers in C.** The `[index] = value` syntax is a
> C99 feature. It lets you initialize specific array elements without
> filling in every element before them. Elements not explicitly
> initialized default to 0 (for integers) or NULL (for pointers):
>
> ```c
> int codes[] = {
>     [2] = 42,    // codes[0]=0, codes[1]=0, codes[2]=42
>     [5] = 99,    // codes[3]=0, codes[4]=0, codes[5]=99
> };
> // Array size = 6 (determined by highest designated index + 1)
> ```
>
> This is much cleaner than `codes[0]=0; codes[1]=0; codes[2]=42; ...`
> and perfect for sparse tables like exception codes.

### Deciding what's recoverable

In our current kernel (no user processes, no paging), the policy is
simple:

| Exception | Recoverable? | Action |
|-----------|:------------:|--------|
| Breakpoint (3) | Yes | Print address, advance `sepc` by detected instruction length, return |
| Ecall from S-mode (9) | Yes* | Print address, advance `sepc` by 4 (always 4-byte), return |
| Everything else | No | Print diagnostics, `panic()` |

*Ecall from S-mode isn't truly "recoverable" in a meaningful sense —
we have no SBI firmware to dispatch to. We just print a diagnostic and
skip past the instruction so the kernel doesn't loop forever. On real
hardware with OpenSBI, this would be an SBI call to firmware. Ecall
from U-mode (code 8, system calls) will be handled in Phase 6.

Once we have user processes (Phase 6), the policy becomes richer:

| Exception | In kernel | In user process |
|-----------|-----------|-----------------|
| Page fault | Panic (for now) | Map the page (COW, lazy alloc), or kill the process |
| Illegal instruction | Panic | Kill the process (send SIGILL) |
| Ecall from U-mode | N/A (can't happen) | Dispatch to syscall handler |
| Access fault | Panic | Kill the process (send SIGSEGV) |

The difference: in user-space, exceptions are expected and the kernel
handles them gracefully. In kernel-space, exceptions (other than
breakpoints) usually mean a bug, so we panic.

This is exactly what xv6 does — we'll see in Part 5.

---

## Part 5: How xv6 Handles Kernel Exceptions

### `kerneltrap()` in xv6

The xv6 book (section 4.5) says it plainly:

> **kerneltrap** is prepared for two types of traps: device interrupts
> and exceptions. It calls **devintr** to check for and handle the
> former. If the trap isn't a device interrupt, it must be an
> exception, and that is always a fatal error if it occurs in the
> xv6 kernel; the kernel calls **panic** and stops executing.

Here's the relevant xv6 code:

```c
// kernel/trap.c — xv6's kerneltrap (simplified)

void
kerneltrap()
{
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    if ((which_dev = devintr()) == 0) {
        // Not an interrupt → must be exception → fatal
        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
        panic("kerneltrap");
    }

    // ... handle device interrupt (timer → yield, etc.) ...

    // Restore sepc and sstatus because yield() may have changed them
    w_sepc(sepc);
    w_sstatus(sstatus);
}
```

xv6's kernel exception handling is literally: print `scause`, `sepc`,
`stval`, panic. No exception name table, no attempt at recovery.
This is fine for xv6 — it's a teaching OS and kernel exceptions mean
bugs.

The interesting exception handling in xv6 lives in `usertrap()`, which
handles traps from user-space. There, ecalls dispatch to the syscall
table, and exceptions kill the faulting process rather than panicking
the whole kernel. We'll build that in Phase 6.

### What we're improving over xv6

Our exception handler will be better than xv6's in one specific way:
**human-readable diagnostics**. When xv6 panics with
`scause 0000000000000002`, you have to look up what cause 2 means.
When bobchouOS panics with `illegal instruction at sepc=0x800001a4`,
you immediately know what happened.

This is purely a developer-experience improvement — it doesn't change
the kernel's behavior (it still panics). But during development, good
error messages save hours of debugging.

> **Why doesn't xv6 bother with readable names?** xv6 prioritizes
> minimalism — every line of code is something students must read and
> understand. An exception name table adds 15 lines and one concept
> (designated initializers, string lookup). For a real teaching OS,
> that's a reasonable tradeoff. For bobchouOS, we're building for
> our own learning, so we can afford nicer diagnostics.

---

## Part 6: Page-Fault Exceptions — A Preview

### Why page faults are special

Most exceptions signal errors. An illegal instruction means bad code.
An access fault means a permission problem. But page faults (codes 12,
13, 15) are different — they're the kernel's most powerful tool for
implementing advanced memory features.

The xv6 book (section 4.6) puts it well:

> Xv6's response to exceptions is quite boring: if an exception
> happens in user space, the kernel kills the faulting process. If an
> exception happens in the kernel, the kernel panics. Real operating
> systems often respond in much more interesting ways.

Here are the three big features that page faults enable:

### Copy-on-write (COW) fork

When a process calls `fork()`, the child gets a copy of the parent's
memory. A naive implementation copies all physical pages immediately —
slow and wasteful, since the child often calls `exec()` right after
and throws it all away.

**COW fork** is clever: both parent and child share the same physical
pages, but they're mapped read-only. When either process tries to
*write*, the CPU raises a **store page fault** (code 15). The kernel's
handler:

1. Allocates a new physical page
2. Copies the faulted page's contents to the new page
3. Updates the faulting process's page table to point to the new page
   (with write permission)
4. Returns — the CPU re-executes the `sd` instruction, which succeeds
   because the new page is writable

```
Before fork:
  Parent PTE: vaddr 0x1000 → paddr 0xA000 (RW)

After COW fork:
  Parent PTE: vaddr 0x1000 → paddr 0xA000 (R-only, COW flag)
  Child  PTE: vaddr 0x1000 → paddr 0xA000 (R-only, COW flag)
                                            (same physical page!)

Parent writes to 0x1000:
  → store page fault (code 15, stval=0x1000)
  → kernel allocates paddr 0xB000
  → copies 0xA000 → 0xB000
  → updates Parent PTE: vaddr 0x1000 → paddr 0xB000 (RW)
  → sret → sd succeeds

Child still maps 0x1000 → 0xA000 (R-only, COW flag)
```

This is a beautiful example of the exception retry mechanism: the store
faults, the handler fixes the page table, `sret` re-executes the
store, and the process never knows anything happened.

### Lazy allocation

When a process calls `sbrk()` (or `mmap()`) to request more memory,
a naive kernel allocates physical pages immediately. But the process
might never use all the memory it requested.

**Lazy allocation** delays physical allocation until the memory is
actually accessed:

1. `sbrk(N)` adjusts the process's virtual memory size but doesn't
   allocate physical pages or create page table entries.
2. When the process accesses the new memory, the CPU raises a **load
   page fault** (code 13) or **store page fault** (code 15).
3. The kernel's handler allocates a physical page, maps it, and
   returns.
4. The CPU re-executes the faulting instruction, which succeeds.

This is standard in Linux. `malloc()` → `mmap()` → lazy allocation
means that `char *buf = malloc(1 * GB)` succeeds instantly even on a
machine with 512 MB of RAM — pages are only allocated as the process
touches them.

### Demand paging

An extension of lazy allocation. When `exec()` loads a program, a
naive kernel reads the entire binary into memory. **Demand paging**
marks all code pages as "not present" in the page table. When the CPU
tries to fetch an instruction from an unmapped code page, it raises
an **instruction page fault** (code 12). The kernel reads just that
page from disk, maps it, and returns. The CPU retries the fetch and
the program runs.

This is why large programs start fast — only the pages they actually
execute get loaded.

### The common pattern

All three features share the same structure:

```
1. Something is intentionally not mapped (or mapped read-only)
2. Access triggers a page fault (codes 12, 13, 15)
3. scause tells the kernel what KIND of access (fetch/load/store)
4. stval tells the kernel WHICH address faulted
5. The kernel maps the right page (allocating, copying, or loading)
6. sret re-executes the faulting instruction → succeeds
7. The process never knew anything happened
```

Without `stval`, the kernel wouldn't know *which address* faulted.
Without the retry mechanism (`sepc` pointing to the faulting
instruction), the instruction would be skipped instead of re-tried.
Both are essential for page fault handling.

> **This is why the RISC-V hardware is designed the way it is.** The
> xv6 book (section 4.7, "Real world") explains the reasoning:
>
> "A driving force is that the RISC-V intentionally does as little as
> it can when forcing a trap, to allow the possibility of very fast
> trap handling."
>
> The hardware saves the minimum state (`sepc`, `scause`, `stval`,
> `sstatus`) and jumps. Everything else is software's responsibility.
> This simplicity enables software to implement all these features
> (COW, lazy alloc, demand paging) without special hardware support.

We won't implement any of this until Phase 4 (page tables). But
understanding the *why* of page faults now helps you see the exception
codes not as error conditions but as the kernel's primary mechanism
for managing memory lazily and efficiently.

---

## Part 7: `sepc` Handling — Getting It Right

### The rules

Let's consolidate the `sepc` rules into one clear reference:

| Exception type | `sepc` points to | Handler should | Why |
|---------------|-----------------|---------------|-----|
| Faults (misalign, access, illegal, page fault) | Faulting instruction | Leave `sepc` alone (if the cause is fixed) or panic | CPU will retry the instruction after `sret` |
| Ecall (8, 9) | The `ecall` instruction | Advance `sepc` by 4 (no compressed ecall exists) | `ecall` succeeded; retrying would loop |
| Breakpoint (3) | The `ebreak` instruction | Detect instruction length and advance `sepc` | `ebreak`/`c.ebreak` succeeded; retrying would loop |
| Interrupts | Next instruction to execute | Leave `sepc` alone | The interrupted instruction already completed |

### The sepc save/restore issue

There's a subtle correctness concern with `sepc` that xv6's
`kerneltrap()` handles explicitly. When `kernel_trap()` is called,
`sepc` holds the address of the trapped instruction. But our handler
might call functions that themselves could be interrupted (if we enable
nested interrupts later). If a nested trap fires, it would overwrite
`sepc` with the *new* trap's address, destroying the outer trap's
return point.

xv6 handles this by saving `sepc` at the start of `kerneltrap()` and
restoring it before returning:

```c
void
kerneltrap()
{
    uint64 sepc = r_sepc();           // save sepc
    uint64 sstatus = r_sstatus();     // save sstatus

    // ... handle trap, possibly call yield() which enables interrupts ...

    w_sepc(sepc);                     // restore sepc
    w_sstatus(sstatus);               // restore sstatus
}
```

For bobchouOS right now, this doesn't matter — we don't enable nested
interrupts (the hardware clears `sstatus.SIE` on trap entry), and we
don't call `yield()`. But we're already saving `sepc` into a local
variable, which is the right pattern. When we add scheduling in Phase
5, we'll need to restore it before returning, just like xv6.

Our handler already reads `sepc` into `sepc_val` at the top. To
advance past the trapping instruction, we write the updated value to
the `sepc` CSR (see the dispatch code in Part 4 for the exact logic).
When `kernel_vec` executes `sret`, the CPU reads `sepc` and jumps to
the address we wrote — the instruction after the `ecall`/`ebreak`.

---

## Part 8: Testing Exception Handling

### How to trigger exceptions on purpose

To test our exception handler, we need to deliberately cause different
exceptions. Here are the simplest ways:

**Illegal instruction (cause 2):**

```c
// Access M-mode CSR from S-mode
asm volatile("csrr t0, mhartid");
```

We already did this in Round 2-2. It's reliable: `mhartid` (CSR
`0xF14`) always requires M-mode.

**Breakpoint (cause 3):**

```c
asm volatile("ebreak");
```

Executes the `ebreak` instruction. Our handler should print the
address and continue.

**Ecall from S-mode (cause 9):**

```c
asm volatile("ecall");
```

Because we delegated all exceptions (`medeleg = 0xffff`), including
bit 9, ecall from S-mode traps to S-mode with `scause = 9`. Our
handler should print a diagnostic, advance `sepc` past the ecall,
and continue. (Ecall from U-mode, cause 8, requires a user-mode
process — we'll test that in Phase 6.)

**Load access fault (cause 5) or store access fault (cause 7):**

```c
// Read from a definitely-invalid physical address
volatile uint64 x = *(volatile uint64 *)0x0;
```

Whether this causes an access fault depends on the PMP configuration
and QEMU's memory map. Address 0x0 is before the CLINT region and
shouldn't map to anything — but some QEMU versions handle this
differently. A more reliable trigger might be needed, or we can skip
this test.

**Page faults (causes 12, 13, 15):**

Can't trigger these because paging is disabled (`satp = 0`). We'll
test them in Phase 4.

### Using the test framework

We have a lightweight test framework (`make test`, see `kernel/test/`).
Exception tests go in a new file
`kernel/test/test_trap.c`:

```c
void test_trap(void) {
    kprintf("[trap]\n");

    // Breakpoint: handler should print address, advance sepc, return
    asm volatile("ebreak");
    TEST_ASSERT(1, "ebreak survived");

    // Ecall from S-mode: handler should print, advance sepc, return
    asm volatile("ecall");
    TEST_ASSERT(1, "ecall from S-mode survived");

    // Illegal instruction: fatal — visual check only
    // Uncomment to test (will panic, stopping all further tests):
    // asm volatile("csrr t0, mhartid");
}
```

The breakpoint and ecall tests use `TEST_ASSERT(1, ...)` after the
trapping instruction — if we reach the assert, the handler worked
correctly. The illegal instruction test is commented out by default
because it panics and kills the test runner. Uncomment it temporarily
to verify the panic message is readable, then comment it back out.

Running `make test` should show:

```
=== bobchouOS test suite ===

[trap]
breakpoint at 0x80000xxx
ecall from S-mode at 0x80000xxx (no SBI handler)

=== results: N passed, 0 failed ===
```

---

## Part 9: How bobchouOS Will Do It

### The plan

We need changes in one file:

**`trap.c`** — extend `kernel_trap()` with:
- An exception name table (string array with designated initializers)
- An `exc_name()` helper function
- A `switch` or `if` chain that dispatches by `scause` value
- Breakpoint handler (non-fatal: print, advance `sepc` by inst_len, return)
- Ecall-from-S-mode handler (non-fatal: print, advance `sepc` by 4, return)
- Default handler (fatal: print readable diagnostics, panic)

We don't need changes to `kernel_vec.S` (the register save/restore
code doesn't change), `entry.S` (the M-mode code is unaffected), or
`riscv.h` (exception cause codes are already defined from Round 2-2).

### File layout

```
kernel/
    trap.c              <-- UPDATE (exception dispatch, name table)
    arch/
        kernel_vec.S    <-- unchanged
        entry.S         <-- unchanged
    include/
        riscv.h         <-- already has EXC_* defines (unchanged)
    main.c              <-- unchanged
    test/
        test_trap.c     <-- NEW (exception handling tests)
        run_tests.c     <-- UPDATE (add test_trap call)
```

### New code structure in `trap.c`

```
trap.c
├── exc_names[]         -- string table: scause code → name
├── exc_name()          -- lookup helper
├── kernel_trap()
│   ├── sanity checks (same as before)
│   ├── interrupt dispatch (same as before)
│   │   └── IRQ_S_SOFT → timer tick
│   └── exception dispatch (NEW)
│       ├── EXC_BREAKPOINT → print, advance sepc by inst_len, return
│       ├── EXC_ECALL_S → print, advance sepc by 4, return
│       └── default → print name + diagnostics, panic
└── ticks counter (same as before)
```

### Test code in `kernel/test/test_trap.c`

```
test_trap()
├── ebreak → TEST_ASSERT survived
├── ecall  → TEST_ASSERT survived
└── (illegal instruction commented out — uncomment for visual check)
```

Tests run via `make test`. The illegal instruction test is kept
commented out by default so it doesn't kill the test runner. Uncomment
it once to verify the panic output is readable, then comment it back.

### Comparison with xv6

| Aspect | xv6 | bobchouOS (Round 2-4) |
|--------|-----|----------------------|
| Kernel exception action | Always panic | Panic for faults; survive breakpoints |
| Exception names | No (raw `scause` hex value) | Yes (string lookup table) |
| `stval` display | Printed as raw hex | Printed as raw hex (same) |
| `sepc` advance for ecall | Done in `usertrap()` for user ecalls | S-mode ecall: done now. U-mode ecall: Phase 6 |
| Breakpoint handling | Not handled (panics) | Print address, advance `sepc`, continue |
| Page fault handling | Kill user process / kernel panic | Kernel panic (Phase 4 will add handling) |
| `devintr()` separation | Yes (separate function) | No (inline in `kernel_trap()`) |
| Exception name table | No | Yes (designated initializer array) |

The difference is small. Both kernels panic on kernel exceptions. Our
improvement is cosmetic — better diagnostic output and a breakpoint
handler. The real exception-handling complexity comes in Phase 6 (user
traps, syscalls) and Phase 4 (page faults).

---

## What's Next

After you read this lecture, we'll:

1. **Create the skeleton** — extend `trap.c` with the exception name
   table, `exc_name()` helper, and a dispatch `switch` with TODOs.
   Add `kernel/test/test_trap.c` with exception tests.
2. **You implement the TODOs** — fill in the exception dispatch logic,
   breakpoint handler, and the fatal exception panic format.
3. **Test** — `make test` should show:
   - The `ebreak` and `ecall` tests passing (survived)
   - Uncomment the illegal instruction test temporarily to verify a
     readable panic: `"kernel_trap: illegal instruction"` with `sepc`
     and `stval`
   - `make run` should still boot normally with timer ticks

This completes Phase 2 (RISC-V Privilege Levels & Traps). After
Round 2-4, we'll have:
- M-mode → S-mode switch (Round 2-1)
- Trap vector + handler (Round 2-2)
- Timer interrupts (Round 2-3)
- Exception handling (Round 2-4)

Phase 3 is next: **Physical Memory Management** — the kernel allocator
(`kalloc`/`kfree`), free lists, and the page allocator that everything
else builds on.

---

## Quick Reference

### Exception cause codes (`scause` bit 63 = 0)

| Code | Name | `stval` contents | Fatal in kernel? |
|:----:|------|-----------------|:----------------:|
| 0 | Instruction misaligned | Faulting address | Yes |
| 1 | Instruction access fault | Faulting address (platform-guaranteed in practice) | Yes |
| 2 | Illegal instruction | Instruction encoding (or 0) | Yes |
| 3 | Breakpoint | `ebreak` address (or 0) | No (print + skip) |
| 4 | Load misaligned | Faulting address | Yes |
| 5 | Load access fault | Faulting address (platform-guaranteed in practice) | Yes |
| 6 | Store/AMO misaligned | Faulting address | Yes |
| 7 | Store/AMO access fault | Faulting address (platform-guaranteed in practice) | Yes |
| 8 | Ecall from U-mode | 0 | N/A (only from U-mode; syscall handler in Phase 6) |
| 9 | Ecall from S-mode | 0 | No (advance `sepc` + skip) |
| 10 | (reserved) | — | — |
| 11 | Ecall from M-mode | 0 | N/A |
| 12 | Instruction page fault | Faulting virtual address (platform-guaranteed in practice) | Yes (until Phase 4) |
| 13 | Load page fault | Faulting virtual address (platform-guaranteed in practice) | Yes (until Phase 4) |
| 14 | (reserved) | — | — |
| 15 | Store/AMO page fault | Faulting virtual address (platform-guaranteed in practice) | Yes (until Phase 4) |

### Interrupt cause codes (`scause` bit 63 = 1)

| Code | Name | Handled by |
|:----:|------|-----------|
| 1 | Supervisor software interrupt | `kernel_trap()` — timer tick (via SSIP forwarding) |
| 5 | Supervisor timer interrupt | Not used (would need SSTC or SBI) |
| 9 | Supervisor external interrupt | Not yet (PLIC, future round) |

M-mode interrupt codes (3, 7, 11) never appear in `scause` — their
`mideleg` bits are hardwired to 0. See Lecture 2-3 for the full
timer interrupt chain.

### `sepc` handling rules

| Trap type | `sepc` adjustment | Why |
|-----------|:-----------------:|-----|
| Fault exceptions (0-2, 4-7, 12-13, 15) | Don't touch | CPU retries the faulting instruction |
| `ecall` (8, 9) | `sepc += 4` (no compressed ecall) | Prevent infinite re-trap (code 11 hardwired to M-mode) |
| `ebreak` (3) | `sepc += inst_len` (detect 4 or 2) | Prevent infinite re-trap |
| Interrupts | Don't touch | Hardware already saved the right address |

### Page fault features (preview — Phase 4+)

| Feature | Page fault codes used | What the handler does |
|---------|:--------------------:|----------------------|
| COW fork | 15 (store) | Allocate new page, copy, remap writable |
| Lazy allocation | 13, 15 (load, store) | Allocate page, map it, zero it |
| Demand paging | 12, 13 (fetch, load) | Load page from disk, map it |

### Instruction length detection

```c
// Read the 16-bit parcel at sepc to determine instruction length.
// RISC-V encoding rule: bits 1:0 both set → 4-byte instruction.
uint16 inst = *(uint16 *)sepc_val;
int inst_len = (inst & 0x3) == 0x3 ? 4 : 2;
```

### Exception dispatch summary (bobchouOS Round 2-4)

```
kernel_trap()
├── scause & SCAUSE_INTERRUPT?
│   ├── yes → interrupt dispatch (same as Round 2-3)
│   │   └── IRQ_S_SOFT → clear SSIP, ticks++, print every 100
│   │
│   └── no → exception dispatch (NEW in Round 2-4)
│       ├── EXC_BREAKPOINT → print sepc, advance sepc by inst_len, return
│       ├── EXC_ECALL_S → print sepc, advance sepc by 4, return
│       └── anything else → print name + sepc + stval, panic
```
