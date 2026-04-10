# Lecture 1-1: Boot Sequence, Linker Scripts, and Kernel Entry

> **Where we are**
>
> Phase 0 gave us the tools (cross-compiler, QEMU, GDB) and the foundations
> (C, assembly, calling conventions). We wrote a trivial bare-metal program
> that printed "Hello from bobchouOS!" by writing bytes to a UART address.
>
> Now we start building the **real kernel**. This lecture covers everything
> that happens from the moment the machine powers on to the moment our C
> function `kmain()` starts running. By the end, you will understand:
>
> - How a RISC-V computer boots
> - What RISC-V privilege modes are and why they matter
> - How an OS kernel is organized (monolithic vs microkernel)
> - What a linker script does and how to write one for a kernel
> - How `entry.S` sets up the world so C code can run
>
> **xv6 book coverage:** This lecture absorbs content from Chapter 2
> (Operating System Organization) and the boot-related parts of Chapter 5
> (Interrupts and Device Drivers).

---

## Part 1: How a Computer Boots

### The cold start problem

When you flip the power switch, the CPU wakes up in a very primitive state:

- All registers are at some default value (often zero or undefined)
- There is no stack
- There is no operating system
- There is no `main()` function to call
- The CPU just starts executing instructions from a fixed address

This creates a **chicken-and-egg problem**: the CPU needs software to do
anything useful, but software needs the CPU to already be running. The
solution is a chain of increasingly sophisticated software, each stage
setting up just enough for the next one:

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  Hardware   │ ──→ │  Firmware   │ ──→ │  Bootloader │ ──→ │   Kernel    │
│  reset      │     │  (ROM)      │     │  (optional) │     │             │
└─────────────┘     └─────────────┘     └─────────────┘     └─────────────┘

CPU wakes up         First software       Loads kernel        Your OS starts
at fixed addr        that runs            into RAM            running
```

On a real RISC-V board, the chain might be:

1. **ROM code** — hardwired in the chip, does minimal hardware init
2. **OpenSBI** — a firmware that runs in Machine mode, provides services
3. **U-Boot** — a bootloader that loads the kernel from disk
4. **Kernel** — your operating system

> **Firmware vs bootloader — what's the difference?**
>
> They're often confused because they overlap in practice.
>
> **Firmware** is software permanently stored in the hardware (ROM/flash).
> It runs first, does the most basic hardware initialization, and provides
> runtime services. It's part of the board — you don't install it, it's
> already there.
>
> **Bootloader** is software that finds and loads the OS kernel into RAM.
> It's more flexible — it might read a kernel from disk, network, or USB,
> and let you choose which OS to boot.
>
> | Stage | Example | Stored in | Job |
> |-------|---------|-----------|-----|
> | Firmware | OpenSBI | ROM/flash on the chip | Hardware init, provide M-mode services, hand off to next stage |
> | Bootloader | U-Boot | Flash or disk | Find the kernel (on disk, network, etc.), load it into RAM, jump to it |
> | Kernel | Linux / bobchouOS | Disk | Run the OS |
>
> The line is blurry — some firmware also loads the kernel directly (acting
> as both), and some bootloaders do hardware init. On simple embedded
> systems, there's often just one blob that does everything.
>
> With our QEMU setup (`-bios none -kernel kernel.elf`), we skip both —
> QEMU itself plays the role of firmware + bootloader by loading our ELF
> into RAM and jumping to it.

### How QEMU simplifies this

QEMU's `virt` machine skips most of this chain. When you run:

```
qemu-system-riscv64 -machine virt -bios none -kernel kernel.elf
```

Here's what happens:

1. QEMU creates a virtual RISC-V computer with RAM, UART, etc.
2. The `-kernel kernel.elf` flag tells QEMU to load our ELF file into RAM
   at address **0x80000000**
3. The `-bios none` flag tells QEMU to skip any firmware
4. QEMU sets each CPU's program counter (PC) to **0x80000000** and starts
   executing

That's it. No firmware, no bootloader — our code is the first thing that
runs. This is great for learning because we control everything from the very
first instruction.

### Why 0x80000000?

You might wonder why RAM starts at `0x80000000` instead of `0x00000000`.

On the QEMU `virt` machine, the lower address range (`0x00000000` to
`0x7FFFFFFF`) is reserved for **memory-mapped I/O devices**. Each device
gets a slice of this address space:

```
Address space of QEMU "virt" machine:

0x00000000 ┌─────────────────────┐
           │   Reserved / ROM    │
0x00001000 ├─────────────────────┤
           │   (various)         │
0x02000000 ├─────────────────────┤
           │   CLINT             │  ← Timer and software interrupts
0x0C000000 ├─────────────────────┤
           │   PLIC              │  ← External interrupt controller
0x10000000 ├─────────────────────┤
           │   UART0             │  ← Serial port (one byte wide)
0x10001000 ├─────────────────────┤
           │   virtio devices    │  ← Virtual disk, network, etc.
           │   ...               │
0x80000000 ├─────────────────────┤
           │                     │
           │   RAM               │  ← Our kernel lives here
           │   (128 MB default)  │
           │                     │
0x88000000 └─────────────────────┘
```

When the CPU writes to `0x10000000`, it doesn't go to RAM — it goes to the
UART hardware. When it writes to `0x80000000`, it goes to RAM. The hardware
routes each address to the right destination. This is called **memory-mapped
I/O (MMIO)** — devices appear as addresses in the memory map.

This is why our linker script starts at `0x80000000` — that's where RAM
begins, and that's where QEMU loads our kernel.

---

## Part 2: RISC-V Privilege Modes

### Why do we need privilege modes?

Imagine you're running a word processor and a web browser at the same time.
If the word processor has a bug that writes to random memory, it could
corrupt the web browser's data — or worse, corrupt the operating system
itself. Without protection, one buggy program can crash everything.

The solution: the CPU itself enforces boundaries. It has different **modes**
(also called **privilege levels**) with different permissions. Regular
programs run in a restricted mode where they can't touch hardware or other
programs' memory. The OS kernel runs in a privileged mode where it can do
anything.

### The three RISC-V modes

RISC-V defines three privilege modes:

```
┌──────────────────────────────────────────────────┐
│  M-mode (Machine mode)                           │  Highest privilege
│  • Full access to everything                     │
│  • Can configure CPU, handle interrupts          │
│  • Typically runs firmware (e.g., OpenSBI)       │
├──────────────────────────────────────────────────┤
│  S-mode (Supervisor mode)                        │  Middle privilege
│  • Can manage virtual memory (page tables)       │
│  • Can handle traps and interrupts               │
│  • Where OS kernels normally run                 │
├──────────────────────────────────────────────────┤
│  U-mode (User mode)                              │  Lowest privilege
│  • Cannot access hardware directly               │
│  • Cannot change page tables                     │
│  • Must ask the kernel via system calls          │
│  • Where normal programs run                     │
└──────────────────────────────────────────────────┘
```

Think of it like a building:

| Mode | Analogy | Who runs here |
|------|---------|---------------|
| M-mode | Building owner — has keys to everything, sets the rules | Firmware |
| S-mode | Building manager — manages rooms, handles requests | OS kernel |
| U-mode | Tenant — can only use their own room, asks manager for help | User programs |

### How the CPU tracks the current mode

The CPU has an internal privilege level — you can think of it as a hidden
2-bit register inside the CPU:

```
┌─────────────────────────────────────┐
│  CPU internal state                 │
│                                     │
│  current_privilege:  2 bits         │
│    00 = User (U-mode)               │
│    01 = Supervisor (S-mode)         │
│    11 = Machine (M-mode)            │
└─────────────────────────────────────┘
```

This is **not** a CSR you can read or write directly. There's no
`csrr a0, current_mode` instruction. The CPU just *knows* what mode it's
in and enforces rules on every sensitive operation:

```
Code tries to:                      CPU checks:
──────────────────────────────      ─────────────────────────────
Execute "csrr a0, satp"          → Am I in S-mode or M-mode? → allow/deny
Execute "csrr a0, mstatus"       → Am I in M-mode? → allow/deny
Execute "mret"                    → Am I in M-mode? → allow/deny
```

If code in U-mode tries `csrr a0, mstatus` (an M-mode CSR), the CPU
raises an **illegal instruction exception** — the code is not allowed to
touch that register.

Since this privilege level is a physical circuit inside the CPU, it's as
trustworthy as the hardware itself. In theory, if a cosmic ray flipped
those 2 bits from `00` (User) to `11` (Machine), a user program would
suddenly have full hardware access. This is a real concern in domains like
aerospace, where radiation-hardened chips and triple modular redundancy
(three copies of every circuit, majority vote) are used. For our QEMU
virtual machine, the hardware is simulated in software — no cosmic rays to
worry about.

### What changes the current mode

Only a few events can change the privilege level:

| Event | Direction | Mechanism |
|-------|-----------|-----------|
| Trap (exception, interrupt, `ecall`) | **Up** (U→S, S→M, or U→M) | CPU automatically switches to higher mode |
| `mret` | **Down** from M-mode | Switches to whatever `mstatus.MPP` says |
| `sret` | **Down** from S-mode | Switches to whatever `sstatus.SPP` says |

There is **no** "set mode to X" instruction. You can only go **up** via a
trap, or **down** via `mret`/`sret`. This is a security feature — user
code cannot simply say "make me kernel mode." It must go through a
controlled entry point (the trap handler).

Also notice: traps only go to the **same or higher** privilege level. A
trap in S-mode never goes to U-mode. You don't want a hardware interrupt
to suddenly give a user program kernel privileges.

| Trap occurs in | Can go to |
|----------------|-----------|
| U-mode | S-mode (if delegated) or M-mode |
| S-mode | S-mode (if delegated) or M-mode |
| M-mode | M-mode only (nowhere higher to go) |

### What each mode can do — CSRs

Each mode has its own set of **Control and Status Registers (CSRs)** —
special CPU registers that control hardware behavior. A key rule: you can
only access CSRs for your mode or below. If code running in U-mode tries
to access an S-mode CSR, the CPU raises an exception.

| CSR prefix | Mode | Example CSRs | Purpose |
|------------|------|-------------|---------|
| `m*` | Machine | `mstatus`, `mtvec`, `mepc`, `mcause` | CPU configuration, interrupt delegation |
| `s*` | Supervisor | `sstatus`, `stvec`, `sepc`, `satp` | Page tables, trap handling |
| (none) | User | `cycle`, `time` | Performance counters (read-only) |

Some important CSRs we'll encounter:

- **`mstatus`** — Machine status register. Contains the MPP field (Machine
  Previous Privilege), which controls what mode `mret` returns to. More on
  this below.
- **`mepc`** — Machine Exception Program Counter. When `mret` executes, the
  CPU jumps to the address stored here.
- **`mhartid`** — Machine Hart ID. Each CPU core (called a "hart" in RISC-V)
  has a unique ID. Hart 0 is typically the bootstrap processor.
- **`satp`** — Supervisor Address Translation and Protection. Controls page
  tables (virtual memory). Writing 0 means "no translation — use physical
  addresses directly."

### The `mstatus.MPP` field

The `mstatus` register deserves a closer look because it plays a key role
in mode transitions. Its **MPP** field (bits 12:11) records what mode the
CPU was in *before* it entered M-mode:

```
mstatus register (simplified):

  63                    12  11                     0
 ┌──────────────────┬──────────┬───────────────────┐
 │  ...other bits   │   MPP    │  ...other bits    │
 │                  │ (2 bits) │                   │
 └──────────────────┴──────────┴───────────────────┘
                      00 = User
                      01 = Supervisor
                      11 = Machine
```

When a trap happens and the CPU enters M-mode, it automatically writes the
*previous* mode into MPP (so it knows where to return). When `mret`
executes, the CPU reads MPP and switches back to that mode.

This is important because, as we'll see next, xv6 writes MPP manually to
trick `mret` into switching to a mode the CPU never came from.

### Preview: traps and trap handlers

We said traps are what *change* the privilege level upward. Let's briefly
understand what they are, since the concept appears repeatedly in this
lecture. Phase 2 covers traps in full detail — this is just enough to
follow the boot sequence.

A **trap** is the CPU's reaction to something that needs special attention.
There are three kinds:

| Type | Trigger | Example |
|------|---------|---------|
| Exception | Something goes wrong during an instruction | Illegal instruction, page fault |
| Interrupt | External hardware needs attention | UART received a character, timer fired |
| System call | User program deliberately asks the kernel for help | `ecall` instruction |

All three work the same way: the CPU stops what it's doing, saves some
state (the current PC into `mepc`/`sepc`, the cause into `mcause`/`scause`),
and jumps to a **trap handler** — a function the kernel registered in
advance by writing its address to the `mtvec` or `stvec` CSR.

A trap handler is just a function, but with a special return instruction:

| | Normal function | Trap handler |
|---|---|---|
| Called by | `call` instruction | CPU automatically (on trap) |
| Returns with | `ret` (jump to `ra`) | `mret` or `sret` (jump to `mepc`/`sepc` + restore mode) |
| Entry point | Address in `call` instruction | Address stored in `mtvec`/`stvec` CSR |

The flow side by side:

```
Normal function call:              Trap handler:

call foo                           (trap occurs — CPU auto-jumps to stvec)
  │                                  │
  ▼                                  ▼
foo:                               trap_handler:
  addi sp, sp, -16                   save all registers
  sd ra, 0(sp)                       figure out what happened (read scause)
  ... do work ...                    ... handle it ...
  ld ra, 0(sp)                       restore all registers
  addi sp, sp, 16
  ret       ← jump to ra            sret     ← jump to sepc + restore mode
```

The key difference: `ret` is just a jump — it doesn't change privilege.
`sret`/`mret` does two things: jump **and** switch back to the previous
privilege level. That's what makes them special.

For Phase 1, we don't configure any trap handling — if something goes
wrong, the CPU just crashes. That's fine for a "hello world" kernel. You'll
write your first trap handler in Phase 2.

### Trap delegation: who handles what

By default, **all traps go to M-mode** — it's the catch-all. But M-mode
firmware doesn't want to deal with every page fault or system call. Those
are the OS kernel's job (S-mode). So RISC-V provides **delegation
registers**:

| CSR | Purpose |
|-----|---------|
| `medeleg` | Machine Exception Delegation — which exceptions go to S-mode |
| `mideleg` | Machine Interrupt Delegation — which interrupts go to S-mode |

Each bit corresponds to a trap type. If the bit is set, that trap goes to
S-mode instead of M-mode:

```
Trap occurs while in U-mode:

  Is this trap type delegated? (check medeleg/mideleg)
     │
     ├── Yes → jump to stvec, switch to S-mode
     │
     └── No  → jump to mtvec, switch to M-mode
```

For example, xv6 sets `medeleg = 0xFFFF` and `mideleg = 0xFFFF`, meaning
"delegate everything to S-mode." But you could be selective — say you want
M-mode firmware to keep handling illegal instruction exceptions (bit 2),
perhaps to emulate instructions the hardware doesn't support:

```c
w_medeleg(0xFFFF & ~(1 << 2));   // Delegate all EXCEPT illegal instruction
```

```
Exception type           Bit    Delegated to
────────────────────     ───    ────────────
Instruction misaligned    0     S-mode ✓
Instruction access fault  1     S-mode ✓
Illegal instruction       2     M-mode ✗  (firmware handles this)
Breakpoint                3     S-mode ✓
Load address misaligned   4     S-mode ✓
...                       ...   S-mode ✓
ecall from U-mode         8     S-mode ✓  (kernel handles syscalls)
```

A real-world use case: some RISC-V chips don't implement the multiply
extension in hardware. The firmware catches the illegal instruction trap in
M-mode, does the multiplication in software, writes the result to the
destination register, and returns. The OS and user code never know the
difference.

Again, the full details of trap handling and delegation come in Phase 2.
For now, just know the model: traps go up in privilege, delegation
registers control *how far* up, and `mret`/`sret` return back down.

### How xv6 uses privilege modes

When xv6 boots on QEMU with `-bios none`, it starts in M-mode. Here's the
full sequence:

```
Power on
   │
   ▼
┌─────────────────────────────────────────┐
│  entry.S (M-mode)                       │
│  • Set up per-CPU stack                 │
│  • Jump to start()                      │
└────────────────────┬────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────┐
│  start.c (M-mode)                       │
│  • Set mstatus.MPP = Supervisor         │
│  • Set mepc = main                      │
│  • Disable paging (satp = 0)            │
│  • Delegate interrupts to S-mode        │
│  • Configure PMP (memory protection)    │
│  • Set up timer                         │
│  • Execute mret ──→ jumps to main()     │
└────────────────────┬────────────────────┘
                     │  (mode switches from M to S)
                     ▼
┌─────────────────────────────────────────┐
│  main.c (S-mode)                        │
│  • Initialize console, memory, etc.     │
│  • Create first user process            │
│  • Start scheduler                      │
└─────────────────────────────────────────┘
```

### The `mret` trick explained

The `mret` instruction was designed for returning from a trap handler:
a trap happens in S-mode, the CPU enters M-mode to handle it, and `mret`
returns back to S-mode. It does this by reading two registers:

- **`mepc`** — the address to jump to
- **`mstatus.MPP`** — the mode to switch to

In normal use:

```
S-mode code running
       │
       ▼  (trap occurs)
CPU automatically:
  1. Saves current PC → mepc
  2. Saves current mode (S) → mstatus.MPP
  3. Switches to M-mode
  4. Jumps to mtvec (trap handler)
       │
       ▼
M-mode trap handler runs, then executes mret
       │
       ▼
CPU reads mepc → jumps back there
CPU reads mstatus.MPP → switches back to S-mode
```

But at boot, there was no trap. We're in M-mode from the start, and we
want to get to S-mode. There's no "please switch to S-mode" instruction —
the *only* way to go from M-mode to a lower mode is `mret`.

So xv6 **fakes a trap return** by manually writing the registers that
`mret` reads:

```
Boot in M-mode (no trap ever happened)
       │
       ▼
start() manually writes:
  1. mstatus.MPP = Supervisor     ← "pretend we came from S-mode"
  2. mepc = address of main()     ← "pretend we were at main()"
       │
       ▼  (executes mret)
CPU reads mepc → jumps to main()
CPU reads mstatus.MPP → switches to S-mode
       │
       ▼
main() runs in S-mode  ✓
```

The CPU doesn't know (or care) that no real trap happened. It just reads
the registers and does what they say. It's like filling out a "return
flight" boarding pass without ever having taken the outbound flight — the
gate agent just scans the pass and lets you through.

### What bobchouOS does in Phase 1

We take a simpler approach for now. We'll stay in **M-mode** for the entire
Phase 1:

```
Power on
   │
   ▼
┌─────────────────────────────────────────┐
│  entry.S (M-mode)                       │
│  • Park all harts except hart 0         │
│  • Zero the BSS section                 │
│  • Set up stack pointer                 │
│  • Jump to kmain()                      │
└────────────────────┬────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────┐
│  main.c (still M-mode)                  │
│  • Initialize UART                      │
│  • Print "hello from bobchouOS"         │
│  • Halt                                 │
└─────────────────────────────────────────┘
```

Why stay in M-mode? Because switching modes is a separate concept with its
own complexity (CSR configuration, interrupt delegation, PMP setup). We'll
do that in Phase 2. For now, M-mode lets us access all hardware directly,
which is exactly what we need to get UART working and print our first
message.

**Key takeaway:** The privilege mode transition (M → S) is conceptually
important but mechanically separate from booting and printing. Phase 1
focuses on getting from power-on to C code to "hello" on the screen.
Phase 2 adds the privilege transition.

---

## Part 3: How an OS Kernel is Organized

Before we build our kernel, let's understand how kernels are typically
structured. This helps explain why we put files where we do.

### The kernel's job

An operating system must provide three things:

1. **Multiplexing** — multiple programs share the same CPU and memory
2. **Isolation** — one program can't crash or spy on another
3. **Interaction** — programs need controlled ways to communicate (pipes,
   files, etc.)

The kernel is the software that implements these guarantees, running in a
privileged mode (S-mode in RISC-V) so it can enforce rules that user
programs cannot bypass.

### Monolithic kernel vs microkernel

There are two main ways to organize a kernel:

**Monolithic kernel** — everything runs in kernel space:

```
┌─────────────────────────────────────────────┐
│                User Space                   │  U-mode
│   shell        editor        browser        │
├─────────────────────────────────────────────┤
│                Kernel Space                 │  S-mode
│   File system    Scheduler    Memory mgmt   │
│   Device drivers    Network stack           │
│   Everything in one big program             │
└─────────────────────────────────────────────┘
```

Pros: fast (no mode switches between subsystems), easy to share data.
Cons: one bug in any driver can crash the entire kernel.

**Microkernel** — minimal kernel, most services in user space:

```
┌─────────────────────────────────────────────────────┐
│                  User Space                         │  U-mode
│  shell    File server    Net server    Disk driver  │
│          (these are just normal processes)          │
├─────────────────────────────────────────────────────┤
│              Microkernel                            │  S-mode
│  (only: message passing, basic scheduling,          │
│   memory management)                                │
└─────────────────────────────────────────────────────┘
```

Pros: more robust (a buggy driver crashes only itself, not the kernel).
Cons: slower (every file read requires message passing between processes).

**Real-world examples:**
- Monolithic: Linux, FreeBSD, xv6
- Microkernel: Minix, L4, QNX (widely used in embedded systems like car
  infotainment)

**bobchouOS and xv6 are both monolithic kernels.** This is the simpler design
and the one used by most Unix-like systems. All our kernel code (memory
management, file system, drivers, scheduler) will run in a single privileged
address space.

### xv6's source organization

The xv6 kernel is about 6,000 lines of C plus a small amount of assembly.
Here are the key files and what they do:

| File | Purpose | Our Phase |
|------|---------|-----------|
| `entry.S` | First boot instructions | Phase 1 |
| `start.c` | M-mode setup, switch to S-mode | Phase 2 |
| `main.c` | Initialize subsystems during boot | Phase 1 |
| `uart.c` | Serial port driver | Phase 1 |
| `printf.c` | Formatted output to console | Phase 1 |
| `string.c` | `memset`, `memmove`, etc. | Phase 1 |
| `kalloc.c` | Physical page allocator | Phase 3 |
| `vm.c` | Page tables and address spaces | Phase 4 |
| `proc.c` | Processes and scheduling | Phase 5 |
| `swtch.S` | Context switch (save/restore registers) | Phase 5 |
| `trap.c` | Handle traps and interrupts | Phase 2 |
| `syscall.c` | System call dispatch | Phase 6 |
| `fs.c` | File system | Phase 7 |
| `console.c` | Connect UART to user keyboard/screen | Phase 8 |

Notice how the files we're building in Phase 1 (entry.S, main.c, uart.c,
printf.c, string.c) appear at the top — they're the foundation everything
else builds on.

### Our file organization

bobchouOS organizes files into subdirectories by function, while xv6 puts
everything flat in `kernel/`. Our structure for Phase 1:

```
kernel/
├── arch/
│   └── entry.S           # CPU-specific: boot code, will also hold
│                         # trap vectors and CSR helpers later
├── drivers/
│   ├── uart.h            # UART public interface
│   └── uart.c            # UART implementation
├── include/
│   ├── types.h           # Shared type definitions
│   └── kprintf.h         # kprintf interface
├── lib/
│   ├── kprintf.c         # Formatted output
│   └── string.c          # memset, memcpy
└── main.c                # Kernel entry point (kmain)
```

Why subdirectories? As the kernel grows (from 5 files now to 20+ files by
Phase 8), grouping by function keeps things navigable. It also makes the
architecture visible — you can see at a glance that `arch/` is
CPU-specific, `drivers/` is hardware, `lib/` is utilities.

---

## Part 4: Linker Scripts — Controlling Where Code Lives

### What the linker does

Remember the build pipeline from Phase 0:

```
source.c ──→ compiler ──→ object.o ──→ linker ──→ program.elf
source.S ──→ assembler ──→ object.o ─┘
```

The compiler and assembler work on one file at a time. They produce
**object files** (`.o`) that contain machine code, but with some blanks:
"I call `uart_init`, but I don't know its address yet."

The **linker** combines all object files into a single executable. It fills
in those blanks (resolving symbols) and decides where each piece of code and
data goes in memory. The **linker script** is the linker's instruction
manual — it tells the linker exactly where to place everything.

### Why we need a linker script

Without a linker script, the linker uses defaults that assume you're
building a Linux program — code starts at some standard address, there's
a C runtime that calls `main()`, etc. None of that applies to us:

| Assumption | Reality |
|-----------|---------|
| Standard load address | Our kernel must start at 0x80000000 |
| C runtime calls `main()` | We have no C runtime; `entry.S` calls `kmain()` |
| OS handles memory layout | We ARE the OS; we decide the layout |
| BSS is automatically zeroed | No one zeros BSS for us; we must do it ourselves |

So we write our own linker script to control everything.

### Linker script syntax

Linker scripts use a domain-specific language (not C, not assembly). The
syntax looks intimidating at first, but there are only a few concepts:

#### The location counter: `.`

The most important concept is the **location counter**, written as a dot
(`.`). It tracks "where am I currently placing things in memory?" Think of
it as a cursor:

```c
. = 0x80000000;    /* Move cursor to this address */
```

After this line, anything we place goes at `0x80000000` and after.

#### Sections

The `SECTIONS` command is where you define the memory layout:

```
SECTIONS
{
    . = 0x80000000;

    .text : {
        *(.text)       /* Put all code sections here */
    }

    .data : {
        *(.data)       /* Put all data sections here */
    }
}
```

The `*(.text)` means "from all input files (`*`), take their `.text`
section." The linker collects all code from all object files and places it
at the current location counter.

#### Symbol definitions

You can create symbols that C code can reference:

```
_bss_start = .;     /* Save current address as a symbol */
.bss : {
    *(.bss)
}
_bss_end = .;       /* Save address after BSS */
```

In C, you can then write:

```c
extern char _bss_start[];
extern char _bss_end[];
```

These aren't real variables — they have no storage. Their **address** is
the value the linker assigned. So `_bss_start` gives you the start address
of the BSS section. This is the same trick that was previewed in the Phase
0-3 lecture.

#### Alignment

The `.` counter can be aligned:

```
. = ALIGN(16);     /* Round up to next 16-byte boundary */
```

Alignment matters because some hardware operations require addresses to be
aligned. RISC-V requires the stack pointer to be 16-byte aligned, for
example.

#### ENTRY

The `ENTRY` directive tells the linker (and debuggers) which symbol is the
first instruction to execute:

```
ENTRY(_start)
```

This doesn't actually control where the CPU jumps — that's determined by
QEMU or the bootloader. But it's good practice because tools like GDB use
it to know where to start.

### Our Phase 0 linker script — a review

Here's what we had in Phase 0 (simplified):

```
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .text : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : {
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(16);
    . += 4096;
    stack_top = .;
}
```

This worked for a single tiny program, but has limitations:

1. **No BSS boundary symbols** — we can't zero BSS in entry.S because we
   don't know where it starts or ends
2. **No kernel boundary symbols** — later phases need to know where the
   kernel ends (to manage free memory after it)
3. **Small stack** — 4KB is fine for a trivial program, but the kernel needs
   more
4. **RWX warning** — the linker warned that our single segment has
   Read+Write+Execute permissions, which is a security concern

### Our Phase 1 linker script — what's new

The Phase 1 linker script addresses all of these:

```
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    _kernel_start = .;

    .text : {
        *(.text.entry)          /* entry.S goes first */
        *(.text .text.*)
    }

    . = ALIGN(4096);
    _text_end = .;

    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(16);

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);

    _bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    _bss_end = .;

    . = ALIGN(16);
    . += 0x4000;                /* 16KB stack */
    stack_top = .;

    _kernel_end = .;
}
```

Let's walk through what's new:

**`_kernel_start` and `_kernel_end`** — boundary symbols that mark the
entire kernel image. In Phase 3 (physical memory allocator), we'll need to
know where the kernel ends so we can declare everything after it as free
memory.

**`_text_end`** — marks where code ends, aligned to a page boundary (4096
bytes). In Phase 4 (virtual memory), we'll want to map code as
read+execute and data as read+write — knowing the boundary enables this.

**`_bss_start` and `_bss_end`** — mark the BSS section (the name stands for
"Block Started by Symbol," from a 1950s IBM 704 assembler directive).
`entry.S` will use these symbols to zero BSS before calling any C code. Why
is this important? In a hosted environment (running on Linux), the OS zeros BSS for
you. In bare metal, BSS memory might contain random garbage from a previous
boot. If C code assumes a global `int counter;` starts at zero (which the C
standard guarantees), we must make that true ourselves.

**16KB stack** — `0x4000` = 16384 bytes. This is what xv6 uses per CPU.
Our `kprintf` will use stack for recursive integer-to-string conversion,
and future phases add deeper call chains. 16KB is a safe size.

**`*(COMMON)`** — this captures "common symbols." In C, if you write
`int x;` (without `static` or `extern`) in multiple files, the compiler
may emit it as a COMMON symbol instead of a regular BSS symbol. Including
`*(COMMON)` in the BSS section ensures these don't get lost.

> **What are COMMON symbols?**
>
> When two files both say `int x;` (no `static`, no `extern`), are they
> the same `x`? Strictly, this is undefined behavior — you should use
> `extern` in one file and define it in the other. But many C compilers
> are lenient: they emit `int x;` as a COMMON symbol, meaning "I want
> this variable, but if someone else also wants it, merge them into one."
> The linker combines all COMMON symbols with the same name into a single
> variable in BSS. COMMON symbols are always global.
>
> | Declaration | Linkage | Section | Shared across files? |
> |---|---|---|---|
> | `static int x;` | Internal (file-private) | BSS | No |
> | `int x;` | External (global) | COMMON → merged into BSS | Yes (merged) |
> | `int x = 5;` | External (global) | .data | Yes (but duplicate = linker error) |
> | `extern int x;` | Declaration only | No storage | References someone else's `x` |
>
> In practice, you won't hit this in bobchouOS because we'll write clean
> code with proper `extern` declarations. But `*(COMMON)` is a safety
> net — and it's what xv6's linker script includes too.

### Examining the output

After building, you can verify the layout with command-line tools:

```bash
# Show sections and their addresses
riscv-none-elf-objdump -h kernel.elf

# Show all symbols and their addresses
riscv-none-elf-nm kernel.elf

# Show program headers (loadable segments)
riscv-none-elf-readelf -l kernel.elf
```

For example, `nm` output should show:

```
0000000080000000 T _start
0000000080000000 T _kernel_start
00000000800XXXXX B _bss_start
00000000800XXXXX B _bss_end
00000000800XXXXX T stack_top
00000000800XXXXX T _kernel_end
```

(The exact addresses depend on how much code we have.)

---

## Part 5: The Kernel Entry Point (entry.S)

### What entry.S must do

`entry.S` is the **very first code** that executes after QEMU jumps to
`0x80000000`. At this point:

- We're in M-mode (full privilege)
- No stack is set up
- BSS may contain garbage
- All harts (CPU cores) are running simultaneously
- No interrupts are configured

Before we can call any C function, `entry.S` must handle four things:

1. **Park extra harts** — only one CPU should do the boot work
2. **Zero the BSS section** — ensure global variables start at zero
3. **Set up the stack pointer** — C functions need a stack
4. **Jump to `kmain()`** — hand off to C code

Let's look at each one.

### Step 1: Hart parking

QEMU's `virt` machine can simulate multiple CPU cores. With `-bios none`,
**all harts start executing at 0x80000000 simultaneously**. If we don't
handle this, two (or more) CPUs will try to set up the same stack, zero the
same memory, and call `kmain()` at the same time — chaos.

The solution: read each hart's ID and let only hart 0 continue. All other
harts go into an infinite `wfi` (wait-for-interrupt) loop:

```asm
    csrr  a0, mhartid      # Read this hart's ID
    bnez  a0, park          # If not hart 0, go to park
    # ... (hart 0 continues with boot)

park:
    wfi                     # Low-power wait
    j     park              # Loop forever
```

The `csrr` instruction reads a Control and Status Register. `mhartid` is
an M-mode CSR that contains the hart's unique ID (0, 1, 2, ...). Since
we're running in M-mode, we can read it directly.

> **Note:** `csrr a0, mhartid` is actually a pseudo-instruction for
> `csrrs a0, mhartid, x0`. The `csrrs` instruction reads a CSR into a
> register while simultaneously setting bits specified by the second source
> register. Using `x0` (the zero register) means "set no bits" — so it's
> a pure read. You'll see this pattern throughout OS code.

xv6 does something different — it lets all harts run and gives each one its
own stack (computed as `stack0 + (hartid+1) * 4096`). We'll adopt that
approach in later phases when we add multi-core support. For now, parking
is simpler.

### Step 2: Zeroing BSS

The BSS section holds uninitialized global variables. The C standard says
they start at zero. In a hosted environment, the OS handles this. In bare
metal, we must do it ourselves.

```asm
    la    t0, _bss_start    # Load start address
    la    t1, _bss_end      # Load end address
bss_clear:
    bgeu  t0, t1, bss_done # If start >= end, we're done
    sd    zero, 0(t0)       # Store 8 bytes of zeros
    addi  t0, t0, 8         # Advance pointer
    j     bss_clear         # Repeat
bss_done:
```

We use `sd` (store doubleword = 8 bytes) instead of `sb` (store byte = 1
byte) for efficiency — we're clearing 8 bytes at a time instead of 1.
Since BSS is 16-byte aligned (from the linker script), this is safe.

The symbols `_bss_start` and `_bss_end` come from the linker script. The
assembler generates relocations (placeholders), and the linker fills in the
actual addresses. The `la` (load address) pseudo-instruction handles this.

### Step 3: Stack setup

Same as Phase 0 — load the stack top address into `sp`:

```asm
    la    sp, stack_top
```

The stack grows downward on RISC-V. `stack_top` points to the highest
address of our 16KB stack region. The first `call` instruction will
decrement `sp` and start using memory below `stack_top`.

```
              ← stack_top (0x800XXXXX)
   ┌────────┐
   │ 16 KB  │  Stack grows downward
   │ stack  │       ↓
   │ region │
   └────────┘ ← stack_top - 0x4000
```

### Step 4: Jump to C

```asm
    call  kmain             # Enter C code
```

We use `call` (not `j` or `jal`) because it's the standard way to call a
function — it saves the return address in `ra`. Not that `kmain` should
ever return, but being correct costs nothing.

### After kmain returns

If `kmain()` ever returns (it shouldn't, but bugs happen), we spin:

```asm
spin:
    wfi
    j     spin
```

This is the same halt pattern as Phase 0. `wfi` puts the CPU in a
low-power state instead of burning cycles in a tight loop.

### The complete entry.S — conceptual layout

Putting it all together:

```asm
.section .text.entry
.global _start

_start:
    # 1. Park non-boot harts
    csrr  a0, mhartid
    bnez  a0, park

    # 2. Zero BSS
    la    t0, _bss_start
    la    t1, _bss_end
bss_clear:
    bgeu  t0, t1, bss_done
    sd    zero, 0(t0)
    addi  t0, t0, 8
    j     bss_clear
bss_done:

    # 3. Set up stack
    la    sp, stack_top

    # 4. Jump to C
    call  kmain

spin:
    wfi
    j     spin

park:
    wfi
    j     park
```

This is approximately 20 instructions. That's all the assembly we need to
boot into C. Everything from here on is C code.

### Comparing with xv6's entry.S

xv6's `entry.S` is even shorter — about 10 lines:

```asm
_entry:
    la sp, stack0
    li a0, 1024*4
    csrr a1, mhartid
    addi a1, a1, 1
    mul a0, a0, a1
    add sp, sp, a0
    call start
spin:
    j spin
```

Key differences:

| | xv6 | bobchouOS Phase 1 |
|---|---|---|
| Hart handling | All harts run, each gets own stack | Park all except hart 0 |
| Stack setup | Computed: `stack0 + (hartid+1)*4096` | Simple: `la sp, stack_top` |
| BSS zeroing | Done in `start.c` (C code) | Done in `entry.S` (assembly) |
| Next stop | `start()` (M-mode C code) | `kmain()` (stays in M-mode) |

Both approaches are valid. xv6 does BSS zeroing in C (`start.c` calls
`memset`) because `start.c` runs before any code that depends on BSS being
zero. We do it in assembly to reinforce the principle: "assembly sets up
the world for C."

---

## Part 6: The Build System (Makefile)

### Why a root-level Makefile?

In Phase 0, each exercise had its own Makefile in its own directory. That
made sense — each exercise was self-contained.

Starting in Phase 1, the kernel is a single program built from multiple
source files across multiple directories. A single root-level Makefile is
the right approach — same as xv6. You run `make` from the project root and
get `kernel.elf`.

The old Phase 0 Makefiles remain untouched. They still build their own
exercises independently.

### What the Makefile does

Our root Makefile supports four targets:

| Command | What it does |
|---------|-------------|
| `make` or `make all` | Compile all sources, link into `kernel.elf` |
| `make run` | Build + launch QEMU |
| `make debug` | Build + launch QEMU with GDB stub (waiting for connection) |
| `make clean` | Remove all `.o` files and `kernel.elf` |

### Build flags review

These are the same flags from Phase 0, but let's review what each one does
now that we're building a real kernel:

| Flag | Purpose |
|------|---------|
| `-march=rv64imac_zicsr` | Target RISC-V 64-bit with Integer, Multiply, Atomics, Compressed extensions, plus Zicsr (CSR instructions like `csrr`). Newer toolchains require Zicsr to be listed explicitly. |
| `-mabi=lp64` | Long and Pointer are 64-bit; no hardware floating-point |
| `-ffreestanding` | Don't assume a hosted environment (no libc, no `main()` convention) |
| `-nostdlib` | Don't link the standard C library |
| `-mcmodel=medany` | Allow code to be located at any address. The default (`medlow`) only handles addresses in the first 2 GB (`0x00000000`–`0x7FFFFFFF`). Our kernel is at `0x80000000` = exactly 2 GB, which is already out of range. |
| `-Wall` | Enable all common warnings |
| `-O2` | Optimize for speed (the compiler may reorder or simplify code) |
| `-g` | Include debug symbols (for GDB) |

New in Phase 1:

| Flag | Purpose |
|------|---------|
| `-fno-strict-aliasing` | Allow casting between pointer types freely. OS code often casts `void*` to `uint64*` etc. Without this flag, the optimizer might break such code. |
| `-I kernel/include -I kernel` | Tell the compiler where to find header files. `#include "types.h"` finds `kernel/include/types.h`, and `#include "drivers/uart.h"` finds `kernel/drivers/uart.h`. |

### How linking works

The Makefile explicitly lists every object file:

```makefile
OBJS = kernel/arch/entry.o \
       kernel/main.o \
       kernel/drivers/uart.o \
       kernel/lib/kprintf.o \
       kernel/lib/string.o
```

Why explicit listing instead of `$(wildcard kernel/**/*.c)`? Because in an
educational project, you should see exactly what gets compiled. There's no
magic. When you add a new source file in a later phase, you add it to this
list.

The linker command:

```makefile
$(LD) -m elf64lriscv -T linker.ld -o $@ $(OBJS)
```

- `-m elf64lriscv` — output format is 64-bit RISC-V ELF (little-endian)
- `-T linker.ld` — use our linker script
- `-o $@` — output file (the target, `kernel.elf`)
- `$(OBJS)` — input object files

### The pattern rules

Instead of writing a compile command for each file, we use **pattern
rules**:

```makefile
%.o: %.c
    $(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
    $(AS) $(ASFLAGS) -c -o $@ $<
```

The `%` is a wildcard. `%.o: %.c` means "any `.o` file depends on the
corresponding `.c` file." The `-c` flag means "compile only, don't link."

These use Make's **automatic variables** — shorthand that avoids repeating
filenames:

| Variable | Meaning | Example |
|----------|---------|---------|
| `$@` | The target | `kernel/main.o` |
| `$<` | The first prerequisite | `kernel/main.c` |
| `$^` | All prerequisites | all `.c` and `.h` files listed |

---

## Part 7: main.c — The Kernel Entry Point

### What kmain() does

`kmain()` is the first C function that runs. For Phase 1, it's simple:

```c
void kmain(void)
{
    uart_init();                                // Initialize the UART hardware
    kprintf("hello from bobchouOS\n");          // The milestone!
    kprintf("kernel: %p - %p\n", ...);          // Print system info

    // Nothing else to do — halt
    while (1)
        ;
}
```

In later phases, `kmain()` will grow to initialize memory, page tables,
processes, the file system, and finally start the scheduler. But every
kernel starts the same way: set up enough to print, then print.

### Why `kmain` instead of `main`?

We call our entry point `kmain` (kernel main) instead of `main` for
clarity. In Phase 6, when we add user-mode programs, those programs will
have their own `main()`. Using `kmain` makes it clear this is the
**kernel's** entry point.

xv6 uses plain `main()` — that works fine because kernel and user code live
in completely separate build systems. Our choice of `kmain` is a personal
preference for readability.

---

## Part 8: Tying It All Together

### The complete boot sequence

Here's the full picture of what happens from `make run` to seeing "hello
from bobchouOS":

```
1. make run
   ↓
2. Compiler: entry.S → entry.o
   Compiler: main.c → main.o
   Compiler: uart.c → uart.o
   Compiler: kprintf.c → kprintf.o
   Compiler: string.c → string.o
   ↓
3. Linker: combines all .o files using linker.ld
   → kernel.elf (code at 0x80000000, stack at the end)
   ↓
4. QEMU loads kernel.elf into RAM at 0x80000000
   All harts start executing at 0x80000000
   ↓
5. entry.S: _start
   - Hart 1, 2, ... → park (wfi loop)
   - Hart 0 continues:
     - Zero BSS (_bss_start to _bss_end)
     - Set sp = stack_top
     - call kmain
   ↓
6. main.c: kmain()
   - uart_init() → configures the 16550 UART hardware
   - kprintf("hello from bobchouOS\n")
     → parses format string
     → calls uart_putc() for each character
     → uart_putc() writes bytes to UART at 0x10000000
   ↓
7. QEMU's virtual UART sends bytes to your terminal
   → You see "hello from bobchouOS"
   ↓
8. kmain() enters infinite loop
   Ctrl-A X to exit QEMU
```

### Memory layout at runtime

The CPU sees a single address space, but different address ranges are
routed by the hardware bus to different destinations. The 0–2 GB range
is **not RAM** — it's device registers, ROM, or unmapped. RAM starts at
2 GB (`0x80000000`).

```
            ┌───────────────────────────────────────────────┐
            │  I/O region (0x00000000 – 0x7FFFFFFF)         │
            │  Not RAM — routed to devices by the bus       │
            │                                               │
0x02000000  │  CLINT         ← timer / software interrupts  │
0x0C000000  │  PLIC          ← external interrupt ctrl      │
0x10000000  │  UART0         ← uart_putc writes here        │
0x10001000  │  virtio        ← virtual disk, network        │
            │  ...                                          │
            ├───────────────────────────────────────────────┤
            │  RAM (0x80000000 – 0x88000000)                │
            │  128 MB, starting at the 2 GB mark            │
            │                                               │
0x80000000  │  ┌──────────────┐ ← _kernel_start, _start     │
            │  │  .text       │  Code (entry.S, main.c...)  │
            │  ├──────────────┤ ← _text_end (page-aligned)  │
            │  │  .rodata     │  String literals, consts    │
            │  ├──────────────┤                             │
            │  │  .data       │  Initialized globals        │
            │  ├──────────────┤ ← _bss_start                │
            │  │  .bss        │  Zeroed by entry.S          │
            │  ├──────────────┤ ← _bss_end                  │
            │  │  16KB stack  │  Stack grows downward ↓     │
            │  ├──────────────┤ ← stack_top (sp starts here)│
            │  └──────────────┘ ← _kernel_end               │
            │                                               │
            │  Free RAM (~128 MB)                           │
            │  (managed starting in Phase 3)                │
            │                                               │
0x88000000  └───────────────────────────────────────────────┘
```

---

## What's Next

You now understand the concepts behind booting a RISC-V kernel. Next steps
for this round:

1. **I'll create the scaffolding** — `Makefile`, `linker.ld`, `types.h`,
   `string.c` (complete files, not exercises)
2. **I'll create skeleton files** — `entry.S` and `main.c` with TODO
   markers for you to fill in
3. **You implement the TODOs** — using what you learned in this lecture
4. **We verify** — `make run` should print a character to confirm the boot
   path works

The UART driver and kprintf are covered in Lectures 1-2 and 1-3. For this
round, `main.c` will use a simple raw UART write (just like Phase 0) to
prove that the boot sequence works.

---

## Quick Reference

### Key addresses (QEMU virt machine)

| Address | What |
|---------|------|
| `0x02000000` | CLINT (timer, software interrupts) |
| `0x0C000000` | PLIC (external interrupt controller) |
| `0x10000000` | UART0 (serial port) |
| `0x80000000` | RAM start (kernel load address) |
| `0x88000000` | RAM end (128 MB default) |

### Key CSRs used in Phase 1

| CSR | Mode | What it does |
|-----|------|-------------|
| `mhartid` | M | Read-only hart (CPU core) ID |

### Linker script symbols

| Symbol | Meaning |
|--------|---------|
| `_kernel_start` | Start of kernel image |
| `_kernel_end` | End of kernel image (including stack) |
| `_text_end` | End of code section (page-aligned) |
| `_bss_start` | Start of BSS (uninitialized data) |
| `_bss_end` | End of BSS |
| `stack_top` | Top of the kernel stack (sp starts here) |

### RISC-V instructions used in entry.S

| Instruction | Meaning |
|------------|---------|
| `csrr rd, csr` | Read CSR into register rd |
| `bnez rs, label` | Branch to label if rs ≠ 0 |
| `bgeu rs1, rs2, label` | Branch if rs1 ≥ rs2 (unsigned) |
| `la rd, symbol` | Load address of symbol into rd |
| `sd rs, offset(base)` | Store 8-byte doubleword to memory |
| `addi rd, rs, imm` | Add immediate to register |
| `call label` | Call function (saves return address in ra) |
| `wfi` | Wait For Interrupt (low-power halt) |
| `j label` | Unconditional jump |
