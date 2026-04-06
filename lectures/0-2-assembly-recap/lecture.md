# Lecture 0-2: Assembly Language Recap

> **Scope and intent**
>
> This is an express tour of assembly language, not a comprehensive reference.
> The goal is to give you enough foundation to **read and write the ~200 lines
> of assembly** that appear in a small OS kernel. We focus on RISC-V, but the
> first few sections cover universal concepts that apply to any architecture.

---

## Part 1: What is Assembly Language?

### The language hierarchy

Every program you write eventually becomes a stream of numbers that the CPU
fetches and executes. Assembly language is the thinnest possible layer of
human-readable text on top of those numbers.

```
C source code            →  high-level, portable, abstracts hardware
         ↓ compiler
Assembly language        →  human-readable, 1:1 with machine instructions
         ↓ assembler
Machine code (binary)    →  raw numbers the CPU actually executes
```

A concrete example — here's one line of C and what it becomes:

```
C:          x = x + 1;
Assembly:   addi  a0, a0, 1
Machine:    0x00150513            (hex)
Binary:     00000000000101010000010100010011   (32 bits)
```

`addi a0, a0, 1` and `0x00150513` represent the **exact same instruction**.
Assembly is just a way for humans to read it without memorizing hex patterns.

### How big is one instruction?

Every RISC-V base instruction is **exactly 32 bits (4 bytes)** — the same
width as the binary string above. This is a **fixed-length encoding**: every
instruction is the same size, which keeps the CPU's instruction decoder
simple.

However, our toolchain targets `rv64imac` — the `c` is the **Compressed
extension**, which adds 16-bit (2-byte) versions of the most common
instructions. So in practice you'll see a mix of 4-byte and 2-byte
instructions. The CPU distinguishes them by the lowest 2 bits of the
instruction word. You'll never write compressed instructions yourself — the
assembler/compiler picks them automatically. But you'll see them in `objdump`
output prefixed with `c.` (like `c.addi`, `c.mv`, `c.ret`).

Compare with other architectures:

| Architecture | Instruction length |
|-------------|-------------------|
| RISC-V (base) | Fixed 4 bytes |
| RISC-V + C extension (what we use) | Mixed 2 or 4 bytes |
| ARM AArch64 | Fixed 4 bytes |
| x86-64 | Variable, 1–15 bytes |

### Why not just write everything in C?

C compiles to machine code — so why bother with assembly at all?

Because C makes **assumptions** about the machine state. Every C function
assumes: the stack pointer is valid, registers hold the right arguments
according to a calling convention, and there's a sane environment to return to.
Someone has to set up that environment in the first place, and that someone
can't use C (chicken-and-egg problem).

**Assembly is needed when:**

| Situation | Example in our OS |
|-----------|-------------------|
| C's assumptions aren't met yet | `entry.S` — setting up the stack pointer at boot |
| You need to access CPU control registers | Reading/writing CSRs like `stvec`, `satp`, `mstatus` |
| You need exact control over register save/restore | `swtch.S` — context switch between processes |
| You need to transition between privilege levels | `trampoline.S` — user ↔ kernel mode switch via `sret` |
| Performance-critical inner loops | Not relevant for us, but common in production kernels |

In xv6-riscv (our reference OS), there are exactly **3 assembly files** among
~6,000 lines of C. Assembly is the exception, not the rule.


### How assembly gets turned into a running program

```
  source.S                            (you write this)
     │
     ▼  assembler (riscv-none-elf-as)
  source.o                            (object file — machine code + metadata)
     │
     ▼  linker (riscv-none-elf-ld)
  program.elf                         (executable — all code placed at final addresses)
     │
     ▼  QEMU or real hardware
  running program
```

When you use `gcc` to compile a `.S` file, it invokes the assembler
automatically — you don't usually call `as` directly. The `.S` extension
(capital S) means "run the C preprocessor first" (so `#include` and `#define`
work), while `.s` (lowercase) means "pure assembly, no preprocessing."


---

## Part 2: Assembly Fundamentals

### Registers — the CPU's working variables

A CPU has a small, fixed set of **registers** — tiny storage slots built
directly into the processor. On RISC-V, there are **32 general-purpose
registers**, each 64 bits wide (on RV64):

```
Register    ABI name    Purpose
────────    ────────    ───────
x0          zero        Hardwired to 0. Writes are discarded.
x1          ra          Return address (where to go after a function returns)
x2          sp          Stack pointer
x3          gp          Global pointer (for linker relaxation, rarely used by us)
x4          tp          Thread pointer (points to per-CPU data in our OS)
x5-x7       t0-t2       Temporaries (caller-saved)
x8          s0/fp       Saved register 0 / frame pointer (callee-saved)
x9          s1          Saved register 1 (callee-saved)
x10-x11     a0-a1       Function arguments; a0 is also the return value
x12-x17     a2-a7       Function arguments
x18-x27     s2-s11      Saved registers (callee-saved)
x28-x31     t3-t6       Temporaries (caller-saved)
```

The "ABI name" column is what you'll actually use in code. Nobody writes
`x10` — you write `a0`.

**Key things to internalize:**

- **`zero` (x0)** — always reads as zero. `addi a0, zero, 42` loads the
  constant 42 into `a0`. This is why RISC-V doesn't need a separate
  "load immediate" instruction for small constants.

- **`sp` (x2)** — the stack pointer. You saw `entry.S` set this up. Every
  function call uses it. Break it and everything dies.

- **`ra` (x1)** — the return address. When you `call` a function, the CPU
  stores the address of the **next instruction** (the return point) in `ra`,
  then jumps to the target. When the function executes `ret`, it jumps back
  to whatever address is in `ra`. There is only one `ra` register, so if a
  function uses `call` (which overwrites `ra`), it must save the old `ra` to
  the stack first and restore it before returning — otherwise the return
  address back to *its* caller is lost. Functions that never `call` anything
  (leaf functions) don't need to save `ra` at all, since nothing overwrites it.

- **`a0`-`a7`** — function arguments. First argument goes in `a0`, second in
  `a1`, etc. Return value comes back in `a0`. (`a1` is only used as a second
  return register for 128-bit values like `__int128` or two-register structs
  — extremely rare in kernel code.)

- **`s0`-`s11`** — "saved" registers. If a function uses these, it **must**
  save the old values to the stack and restore them before returning. This is
  why they matter for context switching.

- **`t0`-`t6`** — "temporary" registers. Functions can trash these freely.
  If you need a value in `t0` to survive a function call, **you** must save it.

The rules for which registers are "saved" vs "temporary" — and who is
responsible for preserving what — are called the **calling convention**. This
is arguably the most important concept for OS assembly work. We cover it fully
in **Part 5**.

Beyond the 32 general-purpose registers, RISC-V has **Control and Status
Registers (CSRs)** — special registers that control CPU behavior (interrupt
enable, page table base, trap handler address, etc.). These are accessed with
special instructions (`csrr`, `csrw`), not regular load/store. We'll use
them heavily starting in Phase 2.


### Data sizes and types

Assembly has no types in the C sense — no `int`, no `char *`, no `struct`.
Everything is just **bits in a register or in memory**. It's up to you (or the
compiler) to interpret those bits correctly.

That said, instructions do operate on specific **widths**:

| Suffix | Width | Bits | C equivalent |
|--------|-------|------|--------------|
| `b` | byte | 8 | `char` |
| `h` | halfword | 16 | `short` |
| `w` | word | 32 | `int` |
| `d` | doubleword | 64 | `long` / pointer |

You'll see these suffixes on load and store instructions — but before we list
them, let's first learn **how to read the syntax**.


### Instruction syntax — how to read assembly grammar

Every assembly instruction follows a fixed pattern. RISC-V has only a handful
of operand patterns, and once you learn them, you can read **any** instruction.

**The general form:**

```
opcode   destination, source(s)
```

The destination (where the result goes) is always the **first** operand. This
is opposite to some other assembly flavors (AT&T x86 puts destination last),
but RISC-V and most modern architectures put destination first.

Operands are separated by **commas**. Whitespace between the opcode and
operands, and around commas, is flexible — `add a0,a1,a2` and
`add  a0, a1, a2` are identical.

**The six operand patterns you'll see:**

**Pattern 1: Three registers** — `op rd, rs1, rs2`

```asm
add  a0, a1, a2       # a0 = a1 + a2
```

Read as: "add the values in `a1` and `a2`, put the result in `a0`."
`rd` = destination register, `rs1` = source register 1, `rs2` = source
register 2. The names `rd`/`rs1`/`rs2` are conventions from the RISC-V spec
— you'll see them in documentation.

**Pattern 2: Two registers + immediate** — `op rd, rs1, imm`

```asm
addi a0, a1, 42       # a0 = a1 + 42
```

Read as: "add the value in `a1` and the constant 42, put the result in `a0`."
An **immediate** (`imm`) is a constant number written directly in the
instruction. The size of this constant is limited — for most instructions it's
12 bits, meaning -2048 to +2047.

**Pattern 3: Load — `op rd, offset(rs1)`**

```asm
ld   a0, 16(sp)       # a0 = memory[sp + 16]
lb   a0, 0(a1)        # a0 = memory[a1 + 0]  (one byte, sign-extended)
```

This is the one that looks unusual. The syntax `offset(register)` means
"take the address in the register, add the offset, and access that memory
location." The parentheses are **required syntax** — they're not optional
and not just grouping. You cannot rearrange this:

```
ld a0, 16(sp)      ✓   correct — load from address sp+16
ld a0, sp(16)      ✗   WRONG — not valid syntax
ld a0, (sp)16      ✗   WRONG — not valid syntax
ld a0, sp, 16      ✗   WRONG — not how loads work
```

If the offset is 0, you still write it: `ld a0, 0(a1)`. Think of it like
array indexing in C: `offset(base)` is similar to `*(base + offset)`.

**Pattern 4: Store — `op rs2, offset(rs1)`**

```asm
sd   a0, 16(sp)       # memory[sp + 16] = a0
sb   a0, 0(a1)        # memory[a1 + 0] = a0  (one byte)
```

Same `offset(register)` syntax as loads, but notice: the **first operand is
the source** (the value to store), not the destination. This is the one
exception to "destination first" — stores don't have a register destination,
they write to memory.

**Pattern 5: Branch — `op rs1, rs2, label`**

```asm
beq  a0, a1, done     # if a0 == a1, jump to "done"
blt  a0, a1, loop     # if a0 < a1, jump to "loop"
```

Two registers to compare, then a label (name) to jump to if the condition
is true. The assembler calculates the distance to the label and encodes it
as an offset — you don't need to calculate it yourself.

**Pattern 6: Upper immediate — `op rd, imm`**

```asm
lui  a0, 0x10000      # a0 = 0x10000 << 12 = 0x10000000
```

Only two operands: a register and a large (20-bit) immediate. Used for
loading large constants and for address calculations.

### Putting it together — reading real code

With these six patterns, you can parse any RISC-V instruction. Let's practice
on two small, realistic functions.

**Example 1:** Sum an array of `long` values until hitting a zero element.

```c
// C version:
long sum_until_zero(long *array) {
    long sum = 0;
    while (*array != 0) {
        sum += *array;
        array++;
    }
    return sum;
}
```

```asm
# Assembly version:
sum_until_zero:                         # a0 = pointer to array
    li   t0, 0                          # pseudo:     sum = 0
loop:
    ld   t1, 0(a0)                      # pattern 3:  t1 = *array
    beq  t1, zero, done                 # pattern 5:  if element is 0, stop
    add  t0, t0, t1                     # pattern 1:  sum += element
    addi a0, a0, 8                      # pattern 2:  array++ (8 bytes per long)
    j    loop                           # pseudo:     repeat
done:
    mv   a0, t0                         # pseudo:     return sum in a0
    ret                                 # pseudo:     jump to ra
```

Every line fits one of the six patterns (or is a pseudo-instruction). No
exceptions, no surprises.


### Memory — load/store architecture

RISC-V is a **load/store architecture**: arithmetic instructions work **only**
on registers, never directly on memory. To add 1 to a value in memory, you
must do it in three steps:

```asm
ld   a0, 0(a1)     # 1. load: memory → register
addi a0, a0, 1     # 2. operate: register → register
sd   a0, 0(a1)     # 3. store: register → memory
```

This is different from x86, where you can write `add [rax], 1` to operate on
memory directly. RISC-V's approach is simpler — it keeps the instruction set
small and regular.

The offset in `offset(register)` is a 12-bit signed immediate, giving a range
of -2048 to +2047. That's it — no complex addressing modes like
`[rax + rbx*4 + 8]` (x86) or `[r0, r1, LSL #2]` (ARM). One register, one
small offset. Simple.

**Sign extension** is important: when you load a smaller value into a 64-bit
register, the CPU must fill the upper bits. `lb` fills them with copies of the
sign bit (preserving negative values). `lbu` fills them with zeros. For
example, if memory contains the byte `0xFF`:

```
lb  a0, 0(a1)   →  a0 = 0xFFFFFFFFFFFFFFFF  (-1 as signed)
lbu a0, 0(a1)   →  a0 = 0x00000000000000FF  (255 as unsigned)
```


### The stack

The **stack** is just regular memory that we use with a discipline:

```
High address
    │
    │   ┌────────────┐  ← sp (before function call)
    │   │ saved ra   │
    │   │ saved s0   │
    │   │ saved s1   │
    │   │ local vars │
    │   ├────────────┤  ← sp (inside function)
    │   │            │  (free space)
    ▼
Low address
```

**Example 2:** A typical function prologue and epilogue — `sum_of_squares(x, y)`
calls `square` twice with different arguments and adds the results:

```c
// C version:
int square(int n) { return n * n; }

int sum_of_squares(int x, int y) {
    int a = square(x);
    int b = square(y);
    return a + b;
}
```

```asm
# Assembly version:
square:                         # square is a leaf — no prologue needed
    mulw a0, a0, a0             # a0 = n * n
    ret

sum_of_squares:
    # ── Prologue ──
    addi sp, sp, -32            # grow stack by 32 bytes (16-byte aligned)
    sd   ra, 24(sp)             # save return address (call will overwrite ra)
    sd   s0, 16(sp)             # save s0 (we'll use it to hold y)
    sd   s1,  8(sp)             # save s1 (we'll use it to hold square(x))

    # ── Body ──
    mv   s0, a1                 # s0 = y  (preserve across calls)

    # a0 already holds x — our caller set it up
    call square                 # a0 = square(x)
    mv   s1, a0                 # s1 = square(x)  (preserve across second call)

    mv   a0, s0                 # set up arg: a0 = y
    call square                 # a0 = square(y)

    addw a0, s1, a0             # a0 = square(x) + square(y)

    # ── Epilogue ──
    ld   s1,  8(sp)             # restore s1
    ld   s0, 16(sp)             # restore s0
    ld   ra, 24(sp)             # restore return address
    addi sp, sp, 32             # shrink stack back
    ret                         # jump to ra
```

Why each saved register is needed:
- **`ra`** — `call square` overwrites `ra` with `sum_of_squares`'s return
  point. Without saving it, we'd lose where to return to *our* caller.
- **`s0`** — holds `y` across the first call. We can't leave it in `a1`
  because `call square` is allowed to trash any caller-saved register.
- **`s1`** — holds `square(x)` so it survives the second `call square`.

Notice that `x` doesn't need saving — it's already in `a0` when we call
`square` the first time. We only save values that must survive *across* a
call.

If this function didn't call anything, we wouldn't need a prologue/epilogue
at all (like `square` itself — just `mulw` / `ret`).

**Why 32 bytes?** We save 3 registers × 8 bytes = 24 bytes, rounded up to 32
for 16-byte stack alignment.

**Alternative: stack-only approach (no s-registers).**
You might wonder: instead of saving the old `s0`/`s1`, copying values in, and
restoring at the end — why not just store `y` and `square(x)` directly to the
stack?

```asm
sum_of_squares_v2:
    addi sp, sp, -32
    sd   ra, 24(sp)             # only ra needs saving now

    sd   a1, 16(sp)             # store y directly to stack
    call square                 # a0 = square(x)
    sd   a0, 8(sp)              # store square(x) to stack
    ld   a0, 16(sp)             # reload y
    call square                 # a0 = square(y)
    ld   t0, 8(sp)              # reload square(x)
    addw a0, t0, a0             # a0 = square(x) + square(y)

    ld   ra, 24(sp)
    addi sp, sp, 32
    ret
```

This works — no `s` registers used, no save/restore for them. Counting the
cost of each version:

```
s-register approach:   6 memory ops + 3 register moves
stack-only approach:   6 memory ops + 0 register moves
```

Same number of memory operations (the s-register saves/restores trade
one-for-one with the direct stack stores/loads). The s-register version just
has 3 extra register moves (~1 cycle each — essentially free).

So for this function, the stack-only approach is slightly faster. But the
s-register approach wins when a value is **reused many times** — if `y` were
needed across 5 calls, the stack-only approach loads from memory 5 times,
while the s-register approach keeps it in `s0` for free after one `mv`.

The compiler makes exactly this tradeoff per variable: frequently reused
values get s-registers, rarely reused values get spilled to the stack. For
hand-written assembly, the s-register pattern is the standard approach because
it's simple, consistent, and always correct.

You already saw the simplest form of stack setup in `entry.S`:
`la sp, stack_top`. That was a one-time bootstrap. The pattern above is what
the **compiler generates** for every C function — this is where understanding
assembly really helps when reading compiler output or debugging.


### Additional notes

**Memory alignment.**
Load/store instructions should use **naturally aligned** addresses — the
address should be a multiple of the data size:

| Instruction | Address should be a multiple of |
|------------|-------------------------------|
| `lb` / `sb` | 1 (any address) |
| `lh` / `sh` | 2 |
| `lw` / `sw` | 4 |
| `ld` / `sd` | 8 |

The RISC-V spec technically allows misaligned access, but implementations may
handle it in hardware (slow), trap to a software handler (very slow), or raise
an exception (crash). QEMU handles it transparently, but real hardware may
not. In practice you never worry about this — the compiler generates aligned
accesses, and when you write assembly by hand, the stack is 16-byte aligned
and you use multiples of 8 as offsets.

**Byte order (endianness).**
RISC-V is **little-endian** — the least significant byte is stored at the
lowest address. For example, `sd` stores a 64-bit register value like
`0x0000000080000034` as:

```
Address     Byte
base+0      0x34    (least significant)
base+1      0x00
base+2      0x00
base+3      0x80
base+4      0x00
base+5      0x00
base+6      0x00
base+7      0x00    (most significant)
```

As long as you load with the same width you stored with, you don't need to
think about byte order. It only matters when interpreting memory byte-by-byte
(e.g., network protocols) or casting between pointer types in C.

**Running out of `s` registers.**
There are 12 callee-saved registers: `s0` through `s11`. If a function needs
more live values across calls than `s` registers available, the compiler
**spills** values to the stack — saves to memory, reloads when needed. In
practice, 12 is generous and running out is rare. The compiler handles this
automatically.


---

## Part 3: RISC-V Instruction Set — Complete Walkthrough

RISC-V's base integer instruction set (RV64I) has about 50 instructions. We'll
cover them all here, grouped by category. For each instruction, you get: the
syntax, what it does in plain language, the C equivalent, and when you'd use
it. Instructions marked with **[OS]** are ones we'll write by hand in our OS;
the rest you'll mainly see in compiler output.

### How to read instruction names

RISC-V mnemonics are abbreviations. Once you see the pattern, they're
self-documenting:

- `beq` = **B**ranch if **EQ**ual, `bne` = **B**ranch **N**ot **E**qual, `blt` = **B**ranch **L**ess **T**han, `bge` = **B**ranch **G**reater or **E**qual
- `jal` = **J**ump **A**nd **L**ink, `jalr` = **J**ump **A**nd **L**ink **R**egister
- `lui` = **L**oad **U**pper **I**mmediate, `auipc` = **A**dd **U**pper **I**mmediate to **PC**
- `sll` = **S**hift **L**eft **L**ogical, `sra` = **S**hift **R**ight **A**rithmetic
- `slt` = **S**et if **L**ess **T**han, `csrr` = **C**ontrol **S**tatus **R**egister **R**ead
- `ecall` = **E**nvironment **Call**
- Suffix `i` = **I**mmediate, `u` = **U**nsigned, `w` = **W**ord (32-bit)

### Arithmetic

These all use **pattern 1** (three registers) or **pattern 2** (two registers +
immediate).

```asm
add   rd, rs1, rs2      # rd = rs1 + rs2
```
Add two registers. The workhorse instruction — used everywhere. C equivalent:
`rd = rs1 + rs2`.

```asm
addi  rd, rs1, imm      # rd = rs1 + imm    (imm: -2048 to +2047)
```
Add a register and a small constant. This is probably the single most common
instruction in RISC-V. It does triple duty:
- **Add a constant**: `addi a0, a0, 1` → `a0++`
- **Copy a register**: `addi a0, a1, 0` → `a0 = a1` (add zero)
- **Load a small constant**: `addi a0, zero, 42` → `a0 = 42` (add to zero register)

The assembler's pseudo-instructions `mv` and `li` (for small values) both
expand to `addi`.

```asm
sub   rd, rs1, rs2      # rd = rs1 - rs2
```
Subtract. Note: there is **no `subi`** instruction. To subtract an immediate,
you use `addi` with a negative number: `addi a0, a0, -1`.

```asm
lui   rd, imm           # rd = imm << 12    (imm: 20-bit unsigned)
```
**Load Upper Immediate.** Places a 20-bit value in bits [31:12] of `rd`, and
zeros out bits [11:0]. This is how you build large constants. The assembler
usually generates this for you via the `li` pseudo-instruction.

Example: to load `0x10000000` (the UART address from Lecture 0-1):
```asm
lui   a0, 0x10000       # a0 = 0x10000 << 12 = 0x10000000
```

To load an arbitrary 32-bit value like `0xDEADBEEF`:
```asm
lui   a0, 0xDEADC       # a0 = 0xDEADC000  (upper 20 bits)
addi  a0, a0, -273      # a0 = 0xDEADBEEF  (lower 12 bits: 0xEEF = -273 signed)
```
You'll never write this by hand — `li a0, 0xDEADBEEF` does it automatically.

```asm
auipc rd, imm           # rd = PC + (imm << 12)
```
**Add Upper Immediate to PC.** Like `lui`, but adds to the current program
counter instead of zero. This is how PC-relative addressing works — used by
the assembler in `la` (load address) and `call` pseudo-instructions. You
won't write this directly.

**Word-sized variants (RV64 only):**

On RV64, registers are 64 bits, but sometimes you're working with 32-bit
`int` values. The `w` suffix performs 32-bit operations and sign-extends the
result to 64 bits:

```asm
addw  rd, rs1, rs2      # rd = sign_extend_32(rs1[31:0] + rs2[31:0])
addiw rd, rs1, imm      # rd = sign_extend_32(rs1[31:0] + imm)
subw  rd, rs1, rs2      # rd = sign_extend_32(rs1[31:0] - rs2[31:0])
```

The compiler uses these when operating on `int` (32-bit) variables. You'll
see `addw` in compiler output but rarely write it by hand — in our OS
assembly we mostly work with 64-bit values (addresses and pointers).


### Bitwise logic

```asm
and   rd, rs1, rs2      # rd = rs1 & rs2     (bitwise AND)
andi  rd, rs1, imm      # rd = rs1 & imm
or    rd, rs1, rs2      # rd = rs1 | rs2     (bitwise OR)
ori   rd, rs1, imm      # rd = rs1 | imm
xor   rd, rs1, rs2      # rd = rs1 ^ rs2     (bitwise XOR)
xori  rd, rs1, imm      # rd = rs1 ^ imm
```

These work exactly like C's `&`, `|`, `^` operators. Bit manipulation is
common in OS code — masking flags, combining bit fields, toggling bits in
control registers.

**Common patterns:**
```asm
andi  a0, a0, 0xFF      # mask to low byte:     a0 = a0 & 0xFF
ori   a0, a0, 0x2       # set bit 1:            a0 = a0 | 0x2
xori  a0, a0, -1        # bitwise NOT:          a0 = ~a0  (XOR with all-ones)
```

Why does XOR with `-1` produce NOT? Two's complement defines `-x = ~x + 1`.
Apply it to `x = 1`: `-1 = ~1 + 1 = 1111...1110 + 1 = 1111...1111` (all
ones). XOR-ing any bit with 1 flips it (`0 ⊕ 1 = 1`, `1 ⊕ 1 = 0`), so XOR
with all-ones flips every bit — which is exactly bitwise NOT.

The pseudo-instruction `not rd, rs` expands to `xori rd, rs, -1`.


### Shifts

```asm
sll   rd, rs1, rs2      # rd = rs1 << rs2    (shift left logical)
slli  rd, rs1, imm      # rd = rs1 << imm
srl   rd, rs1, rs2      # rd = rs1 >> rs2    (shift right logical — fills with 0)
srli  rd, rs1, imm      # rd = rs1 >> imm
sra   rd, rs1, rs2      # rd = rs1 >> rs2    (shift right arithmetic — fills with sign bit)
srai  rd, rs1, imm      # rd = rs1 >> imm
```

Logical shifts (`sll`, `srl`) fill the vacated bits with zeros — like C's
`<<` and `>>` on unsigned values. Arithmetic right shift (`sra`) fills with
copies of the sign bit — like C's `>>` on signed values.

```asm
sllw  rd, rs1, rs2      # 32-bit shift left (word-sized variant)
srlw  rd, rs1, rs2      # 32-bit shift right logical
sraw  rd, rs1, rs2      # 32-bit shift right arithmetic
slliw rd, rs1, imm      # (and their immediate forms)
srliw rd, rs1, imm
sraiw rd, rs1, imm
```

**Common patterns:**
```asm
slli  a0, a0, 12        # multiply by 4096 (page size): a0 = a0 * 4096
srli  a0, a0, 12        # divide by 4096:               a0 = a0 / 4096
```

We'll use shifts constantly for page table manipulation (Sv39 paging breaks
addresses into chunks by shifting).


### Comparison (set if less than)

```asm
slt   rd, rs1, rs2      # rd = (rs1 < rs2) ? 1 : 0    (signed)
slti  rd, rs1, imm      # rd = (rs1 < imm) ? 1 : 0    (signed)
sltu  rd, rs1, rs2      # rd = (rs1 < rs2) ? 1 : 0    (unsigned)
sltiu rd, rs1, imm      # rd = (rs1 < imm) ? 1 : 0    (unsigned)
```

These produce a 1 or 0 result — useful for implementing C comparison
operators and conditional expressions. The compiler uses them; you rarely
write them by hand.

A clever trick the compiler uses: `sltiu rd, rs1, 1` sets `rd` to 1 if
`rs1 == 0`, i.e., it's a "set if equal to zero" (the `seqz` pseudo-instruction).


### Load and store

These use **pattern 3** (load) and **pattern 4** (store) — the
`offset(register)` syntax covered earlier.

**Loads** — read from memory into a register:

```asm
ld   rd, offset(rs1)    # load 8 bytes (doubleword, 64-bit)       [OS]
lw   rd, offset(rs1)    # load 4 bytes (word, 32-bit) sign-extended
lwu  rd, offset(rs1)    # load 4 bytes (word, 32-bit) zero-extended
lh   rd, offset(rs1)    # load 2 bytes (halfword) sign-extended
lhu  rd, offset(rs1)    # load 2 bytes (halfword) zero-extended
lb   rd, offset(rs1)    # load 1 byte sign-extended
lbu  rd, offset(rs1)    # load 1 byte zero-extended                [OS]
```

**Stores** — write from a register into memory:

```asm
sd   rs2, offset(rs1)   # store 8 bytes (doubleword)              [OS]
sw   rs2, offset(rs1)   # store 4 bytes (word)
sh   rs2, offset(rs1)   # store 2 bytes (halfword)
sb   rs2, offset(rs1)   # store 1 byte                            [OS]
```

The `offset` is a 12-bit signed immediate (-2048 to +2047).

**Which ones will we use?**
- `ld` / `sd` — most common. Saving/restoring 64-bit registers to the stack.
  Every function prologue/epilogue uses these.
- `lb` / `lbu` / `sb` — for character-level I/O (reading/writing UART bytes).
- `lw` / `sw` — for 32-bit device registers (some MMIO registers are 32-bit).

**Sign-extended vs zero-extended reminder:**

When you load a value smaller than 64 bits into a 64-bit register, the upper
bits must be filled. The signed variant (`lb`, `lh`, `lw`) copies the highest
bit of the loaded value into all upper bits (preserving the sign for negative
numbers). The unsigned variant (`lbu`, `lhu`, `lwu`) fills upper bits with
zeros.

```
byte in memory: 0xFF

lb  a0, 0(a1)   →  a0 = 0xFFFFFFFFFFFFFFFF  (sign-extended: -1)
lbu a0, 0(a1)   →  a0 = 0x00000000000000FF  (zero-extended: 255)
```

**Sign extension preserves the numeric value.** `0xC3` as a signed byte is -61,
and `0xFFFFFFFFFFFFFFC3` as a signed 64-bit value is also -61. The leading
`F`s don't change the number — they just express the same negative value in a
wider representation. Same for positive numbers: leading zeros don't change
the value either. That's why `lb` exists — so -61 stays -61 when loaded into
a 64-bit register.

If you don't care about the distinction (e.g., loading an ASCII character
that's always 0-127), either works. **When in doubt, use the unsigned variant.**


### Branches (conditional jumps)

These use **pattern 5** — two registers + a label.

```asm
beq   rs1, rs2, label   # if rs1 == rs2, jump to label
bne   rs1, rs2, label   # if rs1 != rs2, jump to label        [OS]
blt   rs1, rs2, label   # if rs1 <  rs2, jump (signed)
bge   rs1, rs2, label   # if rs1 >= rs2, jump (signed)
bltu  rs1, rs2, label   # if rs1 <  rs2, jump (unsigned)
bgeu  rs1, rs2, label   # if rs1 >= rs2, jump (unsigned)
```

The assembler figures out the distance to `label` and encodes it as an offset.
You just write the label name.

**Patterns and tricks:**

There's no `bgt` (branch if greater than). You swap the operands:
```asm
blt  a1, a0, label      # if a0 > a1, jump  (swap rs1 and rs2)
```

Similarly, no `ble` (branch if less or equal):
```asm
bge  a1, a0, label      # if a0 <= a1, jump
```

Common idioms with the `zero` register:
```asm
beq  a0, zero, label    # if a0 == 0, jump  (pseudo: beqz a0, label)
bne  a0, zero, label    # if a0 != 0, jump  (pseudo: bnez a0, label)
```

**How a C `if` statement compiles:**

```c
if (x > 0)
    do_something();
```

```asm
    ble  a0, zero, skip    # if x <= 0, skip over (branch on OPPOSITE condition)
    call do_something
skip:
```

Notice the compiler **inverts** the condition — it branches to skip the body
when the condition is false. This is the standard compilation pattern for `if`.

**How a C `for`/`while` loop compiles:**

```c
while (i < n) {
    body();
    i++;
}
```

```asm
    j    test              # jump to the test at the bottom
loop:
    call body
    addi s0, s0, 1         # i++
test:
    blt  s0, s1, loop      # if i < n, jump back to loop
```

Test at the bottom, branch back to the top. This avoids an extra branch
instruction on every iteration compared to putting the test at the top.

Branches have a 12-bit signed offset, giving a range of roughly +/-4KB from
the branch instruction. For anything farther, the assembler automatically
inserts a trampoline using `jal`.


### Jumps and function calls

```asm
jal   rd, label          # rd = PC + 4; jump to label
```
**Jump And Link.** Saves the address of the next instruction (the return
address) into `rd`, then jumps to `label`. This is the fundamental "call"
mechanism. When `rd` is `ra`, it's a function call. When `rd` is `zero`
(discard the return address), it's an unconditional jump.

```asm
jalr  rd, rs1, offset    # rd = PC + 4; jump to rs1 + offset
```
**Jump And Link Register.** Same as `jal` but the target comes from a register
(plus an optional offset). Used for:
- Returning from functions: `jalr zero, ra, 0` (jump to whatever's in `ra`)
- Indirect calls: function pointers, vtables, computed jumps
- Long-range jumps: when the target is too far for `jal`'s 20-bit offset

**You'll never write these directly.** Instead, use the pseudo-instructions:

| You write | Assembler generates | Meaning |
|-----------|-------------------|---------|
| `j label` | `jal zero, label` | Unconditional jump (no return address saved) |
| `jr rs` | `jalr zero, rs, 0` | Jump to address in register |
| `call label` | `auipc ra, ...` + `jalr ra, ra, ...` | Call function, save return address in `ra` |
| `ret` | `jalr zero, ra, 0` | Return to caller (jump to `ra`) |

`call` expands to two instructions because the target might be far away —
`auipc` computes the upper bits of the offset, `jalr` adds the lower bits.

**[OS] In our OS:** `entry.S` uses `call main` and `j spin`. `swtch.S` uses
`ret`. That's about it.


### CSR (Control and Status Register) instructions

These are **unique to privileged / OS-level code**. Normal user programs never
use them.

CSRs are special registers that control CPU behavior: interrupt enable,
trap handler address, page table base, current privilege mode, etc. They
are **not** part of the 32 general-purpose registers — they live in a
separate address space and can only be accessed with these dedicated
instructions.

```asm
csrrw  rd, csr, rs1     # rd = csr; csr = rs1     (read old value, write new)
csrrs  rd, csr, rs1     # rd = csr; csr |= rs1    (read, then set bits)
csrrc  rd, csr, rs1     # rd = csr; csr &= ~rs1   (read, then clear bits)
csrrwi rd, csr, imm     # rd = csr; csr = imm     (immediate versions)
csrrsi rd, csr, imm     # rd = csr; csr |= imm
csrrci rd, csr, imm     # rd = csr; csr &= ~imm
```

These are the **real** instructions. In practice, you'll use pseudo-instructions
that are simpler:

```asm
csrr  rd, csr           # rd = csr                (just read)      [OS]
csrw  csr, rs1          # csr = rs1               (just write)     [OS]
csrs  csr, rs1          # csr |= rs1              (set bits)       [OS]
csrc  csr, rs1          # csr &= ~rs1             (clear bits)     [OS]
```

`csrr rd, csr` is actually `csrrs rd, csr, zero` — read the CSR and "set"
no bits (since the source is zero). `csrw csr, rs1` is `csrrw zero, csr, rs1`
— write the CSR and discard the old value.

**CSR names you'll encounter:**

| CSR | Full name | What it does |
|-----|-----------|-------------|
| `mstatus` | Machine Status | Global interrupt enable, privilege state |
| `mtvec` | Machine Trap Vector | Where to jump on M-mode traps |
| `mepc` | Machine Exception PC | PC saved on M-mode trap |
| `sstatus` | Supervisor Status | S-mode interrupt enable |
| `stvec` | Supervisor Trap Vector | Where to jump on S-mode traps |
| `sepc` | Supervisor Exception PC | PC saved on S-mode trap |
| `satp` | Supervisor Address Translation | Page table base address |
| `scause` | Supervisor Cause | Why the trap happened |

We'll introduce each CSR when we need it. For now, just know that they
exist and that `csrr` / `csrw` are how you access them.


### System instructions

A few special instructions that don't fit the other categories:

```asm
ecall                    # Environment Call — trap into higher privilege level  [OS]
ebreak                   # Breakpoint — trap for debugger
mret                     # Return from M-mode trap handler                     [OS]
sret                     # Return from S-mode trap handler                     [OS]
wfi                      # Wait For Interrupt — halt until interrupt arrives    [OS]
sfence.vma               # Flush TLB (after changing page tables)              [OS]
```

`ecall` is how user programs make system calls — it causes a controlled trap
into the kernel. `sret` / `mret` return from trap handlers. `wfi` puts the
CPU to sleep (you already saw this in `entry.S`'s spin loop). `sfence.vma`
flushes the translation lookaside buffer (TLB) — needed after changing page
tables in Phase 4.

These take no operands (or very specific ones). Just the opcode by itself.


### Fence (memory ordering)

```asm
fence                    # Memory fence — ensure all prior memory operations complete
```

On a single-core system (which is what we're building), `fence` rarely
matters. On multi-core systems, it prevents memory operations from being
reordered across the fence. We won't need this until/unless we add SMP
support.


### Multiplication and division (M extension)

Our toolchain targets `rv64imac` — the `m` means the multiply/divide
extension is available:

```asm
mul    rd, rs1, rs2     # rd = rs1 * rs2           (lower 64 bits of result)
mulh   rd, rs1, rs2     # rd = (rs1 * rs2) >> 64   (upper 64 bits, signed)
mulhu  rd, rs1, rs2     # rd = (rs1 * rs2) >> 64   (upper 64 bits, unsigned)
div    rd, rs1, rs2     # rd = rs1 / rs2           (signed)
divu   rd, rs1, rs2     # rd = rs1 / rs2           (unsigned)
rem    rd, rs1, rs2     # rd = rs1 % rs2           (signed remainder)
remu   rd, rs1, rs2     # rd = rs1 % rs2           (unsigned remainder)
```

Plus `mulw`, `divw`, `divuw`, `remw`, `remuw` for 32-bit variants.

You won't write these by hand — the compiler generates them from C's `*`,
`/`, `%` operators. Listed here for completeness so you recognize them in
disassembly.


### Atomic instructions (A extension)

The `a` in `rv64imac` enables atomic memory operations, needed for
synchronization:

```asm
lr.d   rd, (rs1)        # Load-Reserved: rd = memory[rs1], set reservation
sc.d   rd, rs2, (rs1)   # Store-Conditional: if reservation valid, memory[rs1] = rs2
                         #   rd = 0 on success, nonzero on failure
amoswap.d rd, rs2, (rs1) # Atomic swap: rd = memory[rs1]; memory[rs1] = rs2
amoadd.d  rd, rs2, (rs1) # Atomic add:  rd = memory[rs1]; memory[rs1] += rs2
```

`lr.d` / `sc.d` together implement compare-and-swap, the building block for
spinlocks and other synchronization primitives. We'll use these when
implementing locks (Phase 5). For now, just know they exist.

Note the slightly different syntax: `(rs1)` instead of `offset(rs1)` — atomic
instructions don't take an offset, always offset 0.


### Pseudo-instructions — assembly shortcuts

The assembler provides convenient shorthands that expand to one or two real
instructions. **You should use these in your code** — they're clearer and the
assembler handles the details.

| You write | Expands to | Meaning |
|-----------|-----------|---------|
| `li rd, imm` | `lui` + `addi` (or just `addi` for small values) | Load any constant into `rd` |
| `la rd, symbol` | `auipc` + `addi` | Load address of a label/symbol |
| `mv rd, rs` | `addi rd, rs, 0` | Copy one register to another |
| `not rd, rs` | `xori rd, rs, -1` | Bitwise NOT |
| `neg rd, rs` | `sub rd, zero, rs` | Negate (two's complement) |
| `j label` | `jal zero, label` | Unconditional jump |
| `jr rs` | `jalr zero, rs, 0` | Jump to address in register |
| `call label` | `auipc ra, ...` + `jalr ra, ra, ...` | Call function |
| `ret` | `jalr zero, ra, 0` | Return from function |
| `nop` | `addi zero, zero, 0` | Do nothing |
| `beqz rs, label` | `beq rs, zero, label` | Branch if zero |
| `bnez rs, label` | `bne rs, zero, label` | Branch if not zero |
| `seqz rd, rs` | `sltiu rd, rs, 1` | Set if equal to zero |
| `snez rd, rs` | `sltu rd, zero, rs` | Set if not zero |

`call main` is far more readable than `auipc ra, %hi(main)` /
`jalr ra, ra, %lo(main)`. Same machine code, much clearer intent.


---

## Part 4: Assembly Syntax and Program Structure

### Anatomy of an assembly line

Every line in an assembly file follows this pattern:

```
[label:]    [instruction/directive]    [operands]    [# comment]
```

All four parts are optional. Examples:

```asm
_start:                              # label only
    addi sp, sp, -16                 # instruction with operands
    sd   ra, 8(sp)                   # instruction with operands and implicit comment
loop:   j loop                       # label + instruction on same line
    # This is a comment-only line
                                     # blank lines are fine
```

Labels are just names for addresses. When the assembler sees `loop:`, it
records "the address of the next instruction is called `loop`." Any branch or
jump to `loop` will be resolved to that address.


### Assembler directives

Directives start with `.` and are commands to the assembler — they don't
generate CPU instructions. They control **where things are placed** and **what
metadata to emit**.

**Section directives** — tell the linker which region this code/data belongs to:

```asm
.section .text          # executable code
.section .rodata        # read-only data (string literals, constants)
.section .data          # initialized global variables
.section .bss           # uninitialized global variables (zeroed)
.section .text.entry    # custom section (used in our linker script trick)
```

**`.data` vs `.bss`** — both hold global variables, but `.data` stores initial
values in the ELF file while `.bss` just records "allocate N bytes and zero
them." In C terms: `int counter = 42;` goes in `.data` (the value 42 must be
stored somewhere), while `int buffer[1024];` goes in `.bss` (just needs to be
zeroed at startup). A 64KB buffer in `.bss` costs nothing in binary size; in
`.data` it would embed 64KB of zeros. Our `entry.S` stack (`.space 4096`)
uses `.bss` — 4KB at runtime, zero bytes on disk.

**`.text.entry`** is a custom subsection we invented — the name is arbitrary
(could be `.text.banana`). We need `_start` at exactly `0x80000000` (where
QEMU begins execution), so we put it in its own section. The linker script
then places it first: `*(.text.entry)` before `*(.text)`. Without this, the
linker might put some other function at `0x80000000` and `_start` somewhere
in the middle.

**Symbol visibility:**

```asm
.global _start          # make _start visible to the linker (other files can reference it)
.local  helper          # keep helper private to this file
```

**Alignment:**

```asm
.align 2                # align next item to 2^2 = 4-byte boundary
.align 4                # align to 2^4 = 16-byte boundary (for stack)
```

Note: `.align` on RISC-V uses **powers of two**. `.align 2` means 4-byte
alignment, not 2-byte. This trips people up coming from x86 where `.align 4`
means 4-byte alignment directly.

**Data definition:**

```asm
.byte   0x48, 0x65      # emit raw bytes (0x48 = 'H', 0x65 = 'e')
.half   0x1234           # emit 16-bit value
.word   0xDEADBEEF       # emit 32-bit value
.dword  0x80000000       # emit 64-bit value
.string "Hello\n"        # emit null-terminated string
.space  4096             # emit 4096 zero bytes (reserve space)
.zero   64               # same as .space — emit N zero bytes
```

**Other common directives:**

```asm
.equ    UART_BASE, 0x10000000    # define a constant (like #define in C)
.set    STACK_SIZE, 4096         # same as .equ
.type   _start, @function        # tell the linker this symbol is a function
                                 # (helps with debugging/disassembly)
```

**`.equ` constants vs `.rodata` data.** `.equ` defines an assembly-time
substitution — the name is replaced with the value in each instruction, then
disappears. No memory is used at runtime. `.rodata` holds real bytes at a
real address in memory. Think of `.equ` as C's `#define` (compile-time) and
`.rodata` as C's `const char msg[] = "Hello"` (runtime data you can point to).


### A complete assembly program — structure

Here's the general skeleton of an assembly source file:

```asm
# ──── Header ────
# Comments explaining what this file does

# ──── Constants / macros ────
.equ UART_BASE, 0x10000000

# ──── Code section ────
.section .text
.global my_function

my_function:
    # prologue
    addi sp, sp, -16
    sd   ra, 8(sp)

    # body
    # ...

    # epilogue
    ld   ra, 8(sp)
    addi sp, sp, 16
    ret

# ──── Read-only data section ────
.section .rodata
message:
    .string "Hello\n"

# ──── Mutable data section ────
.section .data
counter:
    .word 0

# ──── Uninitialized data section ────
.section .bss
buffer:
    .space 256
```

Compare this to a C file: the `.text` section is where your functions go,
`.rodata` is for `const` globals and string literals, `.data` is for
initialized globals, and `.bss` is for uninitialized globals. Same layout,
just explicit.


---

## Part 5: The Calling Convention

This is arguably the **most important section** for OS development. The
calling convention is the contract between caller and callee that makes
function calls work.

### The rules

| Rule | Details |
|------|---------|
| **Arguments** | First 8 in `a0`-`a7`. More arguments go on the stack (rare in kernel code). |
| **Return value** | In `a0` (and `a1` for 128-bit values). |
| **Caller-saved** | `ra`, `a0`-`a7`, `t0`-`t6`. If you need these after a call, save them yourself. |
| **Callee-saved** | `sp`, `s0`-`s11`. If you use these, you must restore them before returning. |
| **Stack alignment** | `sp` must be 16-byte aligned at a function call boundary. |

**Why 16-byte stack alignment?** This is a hard requirement from the RISC-V
ABI spec, not just a suggestion. `sp` must be 16-byte aligned at every
function call boundary, which in practice means frame sizes are a multiple
of 16 — that's why `sum_of_squares` allocates 32 bytes (3 registers × 8 = 24,
rounded up to 32) instead of just 24. The ABI chose 16 (not 8) to accommodate
future extensions that may need 128-bit aligned stack data (e.g., the
quad-precision float extension). On QEMU, breaking this rule probably won't
crash, but on real hardware with such extensions it would.

### What "caller-saved" vs "callee-saved" means in practice

Say function `A` calls function `B`:

```
caller-saved (a0-a7, t0-t6):
    A must assume B will destroy these.
    If A needs a value in t0 after calling B, A saves t0 to the stack
    BEFORE calling B, then restores it AFTER.

callee-saved (s0-s11):
    B promises to preserve these.
    If B wants to use s0, B saves the old s0 in its prologue and
    restores it in its epilogue. A never worries about it.
```

This is why `swtch.S` (context switch in Phase 5) only saves `s0`-`s11` and
`ra` — the caller-saved registers were already saved by the C code that called
`swtch()`. The convention divides the work between caller and callee so
nothing is saved twice.

You've already seen the calling convention in action — Example 2
(`sum_of_squares`) in Part 2 demonstrates all the key patterns: saving
`ra` and `s` registers in the prologue, using `s` registers to hold values
across calls, and restoring everything in the epilogue. The leaf function
`square` shows the opposite case: no `call` inside, so no prologue needed.
Every C function the compiler generates follows one of these two patterns.


---

## Part 6: Inline Assembly in C

Most assembly in our OS won't be in `.S` files — it'll be **inline assembly**
embedded in C code. This is how you access CSRs and execute special
instructions without leaving C.

### The syntax

GCC inline assembly uses this format:

```c
asm volatile ("instruction" : outputs : inputs : clobbers);
```

The four parts:

| Part | What it is | Example |
|------|-----------|---------|
| instruction | The assembly template string | `"csrr %0, stvec"` |
| outputs | C variables that receive results | `"=r"(result)` |
| inputs | C variables passed as operands | `"r"(value)` |
| clobbers | Registers/memory the instruction destroys | `"memory"` |

`%0`, `%1`, etc. refer to operands in order (outputs first, then inputs).

`volatile` tells the compiler: "do not optimize this away or reorder it."
Always use `volatile` for CSR access and hardware interaction.

### Practical patterns you'll see

**Read a CSR:**

```c
static inline uint64 r_stvec() {
    uint64 x;
    asm volatile("csrr %0, stvec" : "=r"(x));
    return x;
}
```

This reads the `stvec` register (trap vector address) into C variable `x`.
`"=r"(x)` means: `%0` is an output (`=`), use any general register (`r`),
and store it in `x`.

**Write a CSR:**

```c
static inline void w_stvec(uint64 x) {
    asm volatile("csrw stvec, %0" : : "r"(x));
    return;
}
```

No outputs (first `:` is empty). One input: the value to write.

**Read-modify-write a CSR:**

```c
static inline void enable_interrupts() {
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));
}
```

`csrs` = "CSR set" — sets specific bits without touching others.

### Why `static inline`?

**`inline`** tells the compiler to substitute the function body at each call
site instead of generating an actual function call. The result is a single
`csrr` instruction right where you need it — no call overhead, no stack frame.
This only works if the compiler can **see the function body** at the call site.
C compiles each `.c` file separately, so an inline function defined in `foo.c`
can only be inlined within `foo.c`. Putting it in a header file means every
`.c` that includes the header sees the body and can inline it. That's why
small utility functions like our CSR helpers live in headers.

**`static`** makes the function file-private — visible only within the file
that defines it. This solves a practical problem: since the header gets
`#include`d into multiple `.c` files, without `static` you'd get "multiple
definition" linker errors — every `.c` file would export the same function
name. `static` makes each copy file-private, so the linker never sees a
conflict. In practice the copies all get inlined away and never exist in the
binary, but `static` is still needed in case the compiler decides not to
inline.

**When to use it:** only for tiny functions (1-3 instructions) where the
function call overhead would cost more than the function itself. For large
functions, inlining everywhere would bloat the binary.


---

## Part 7: Building and Examining Assembly

### How to assemble and link

For a standalone `.S` file:

```bash
# Assemble: .S → .o
riscv-none-elf-gcc -c -march=rv64imac -mabi=lp64 my_file.S -o my_file.o

# Link: .o → .elf
riscv-none-elf-ld -T linker.ld my_file.o -o my_program.elf
```

In our project, we'll always use `gcc` (not `as` directly) because our `.S`
files use C preprocessor features like `#include` and `#define`. Running `as`
directly would choke on those — `gcc` runs the preprocessor first, then
passes clean assembly to `as`. The Makefile handles all of this — you just
type `make`.

### How to run it

You can't run a bare-metal ELF directly — there's no OS to load it. Use QEMU
to emulate the hardware:

```bash
qemu-system-riscv64 -machine virt -bios none -nographic -kernel my_program.elf
```

- `-machine virt` — emulate the RISC-V "virt" board (the standard dev board)
- `-bios none` — skip firmware, jump straight to our code at `0x80000000`
- `-nographic` — no GUI window, UART output goes to your terminal
- `-kernel my_program.elf` — load our ELF into memory

This is exactly what we did in Lecture 0-1's bare-metal hello. The Makefile
wraps this into `make qemu`, but the raw command above is what it runs. On
real hardware, you'd flash the binary to a board instead.

### How to examine the output

**Disassemble an ELF file:**

```bash
riscv-none-elf-objdump -d my_program.elf
```

This shows you the machine code alongside the assembly mnemonics — exactly
what the CPU will execute.

If you compiled with `-g` (which embeds debug info — a mapping from each
instruction back to the C source line that generated it), you can add `-S` to
objdump to **interleave the original C source** between the disassembled
instructions:

```bash
riscv-none-elf-objdump -d -S my_program.elf
```

Instead of just seeing `addw a0, a0, a1` / `ret`, you'd see the C line
`int add(int a, int b) { return a + b; }` right above it. Very useful for
understanding which C code produced which instructions.

**See the section layout:**

```bash
riscv-none-elf-objdump -h my_program.elf
```

Shows where each section (`.text`, `.data`, `.bss`) ended up in memory.

**Examine symbols (labels and their addresses):**

```bash
riscv-none-elf-nm my_program.elf
```

Shows every symbol and its address. Useful for verifying that `_start` is at
`0x80000000`.

**Read raw hex:**

```bash
riscv-none-elf-objdump -s -j .text my_program.elf
```

Shows the raw bytes in the `.text` section. Rarely needed, but helps when
you're debugging the assembler/linker itself.

### Going from C to assembly (compiler output)

A useful learning technique: write C code, then look at what the compiler
generates.

```bash
riscv-none-elf-gcc -S -O2 -march=rv64imac -mabi=lp64 my_file.c -o my_file.s
```

The `-S` flag stops after compilation and outputs assembly (`.s` file) instead
of object code. The `-O2` flag enables optimizations so you see realistic
output (unoptimized output is extremely verbose and hard to read).

**Optimization levels:**

| Flag | What it does |
|------|-------------|
| `-O0` | No optimization (default). Every variable lives on the stack — easiest to debug with GDB since variables are always in memory, but the output is bloated and hard to read. |
| `-O1` | Basic optimizations. Some register allocation, dead code removal. Faster compile than `-O2`. |
| `-O2` | Most optimizations enabled. The compiler runs 20+ passes — liveness analysis, register allocation, constant propagation, inlining, and more. This is what we use for reading compiler output. |
| `-O3` | Aggressive. Adds loop unrolling, auto-vectorization. Can make code *larger* for marginal speed gains. |
| `-Os` | Optimize for **size** — like `-O2` but avoids optimizations that increase code size. Common in embedded and kernel code where memory is tight. |

The jump from `-O0` to `-O2` is dramatic. A simple function that puts every
local on the stack at `-O0` might compile to just two instructions at `-O2`
once the compiler applies register allocation and eliminates redundant
loads/stores — the same tradeoffs we discussed with s-registers vs stack.


---

## Part 8: Assembly in Our OS — The Big Picture

Now that you have the fundamentals, here's where assembly appears in the
project ahead:

| Phase | File | What it does | Lines |
|-------|------|-------------|-------|
| 0 | `entry.S` (already written) | Set stack pointer, call main | ~15 |
| 1 | `entry.S` (extended) | Same, slightly more setup | ~20 |
| 2 | Inline asm in `riscv.h` | CSR read/write helpers | ~30 |
| 2 | `kernelvec.S` | Trap entry/exit from kernel mode | ~30 |
| 5 | `swtch.S` | Save/restore registers for context switch | ~20 |
| 6 | `trampoline.S` | User ↔ kernel mode transition | ~50 |

Total: **~170 lines of assembly** across the entire OS.

Everything else is C. Assembly is the escape hatch, not the main road. When
we reach each of these files, we'll walk through them line by line — the
foundation from this lecture will make that straightforward.


---

## Exercises

Four hands-on exercises in the `exercises/` directory let you practice what
you've learned. See `exercises/GUIDE.md` for instructions.

| # | Function | What it tests |
|---|----------|--------------|
| 1 | `add_three(a, b, c)` | Arithmetic, calling convention basics |
| 2 | `strlen(s)` | Loop, byte load, pointer walking |
| 3 | `max_array(arr, n)` | Loop, doubleword load, conditional branch |
| 4 | `sum_strlen(s1, s2)` | Prologue/epilogue, `call`, saving registers |

Exercises 1-3 are leaf functions (no prologue needed). Exercise 4 calls
`strlen` twice, requiring the full prologue/epilogue pattern from Example 2.


---

## Quick Reference Card

### Registers
```
zero (x0)   always 0
ra   (x1)   return address
sp   (x2)   stack pointer
a0-a7       function arguments; a0 is also the return value
s0-s11      callee-saved (must preserve)
t0-t6       temporaries (may trash)
```

### Common instructions
```
add   rd, rs1, rs2      add registers
addi  rd, rs, imm       add immediate
sub   rd, rs1, rs2      subtract
and/or/xor              bitwise ops
slli/srli               shift left/right
lui   rd, imm           load upper 20 bits
ld/sd rd, off(rs)       load/store 64-bit
lw/sw rd, off(rs)       load/store 32-bit
lb/sb rd, off(rs)       load/store 8-bit
beq/bne                 branch equal/not
blt/bge                 branch less/greater
jal/jalr                jump and link
csrr/csrw/csrs/csrc     CSR read/write/set/clear
ecall                   trap to higher privilege
```

### Pseudo-instructions
```
li rd, imm              load constant
la rd, sym              load address
mv rd, rs               copy register
call label              call function
ret                     return
j label                 unconditional jump
nop                     do nothing
```

### Directives
```
.section .text/.data/.bss/.rodata    section placement
.global sym                          make symbol visible
.align N                             align to 2^N bytes
.word/.dword/.byte/.string           emit data
.equ NAME, value                     define constant
.space N                             reserve N bytes
```
