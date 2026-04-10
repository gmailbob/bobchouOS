# Lecture 0-1: Tooling Setup for RISC-V OS Development

> **Heads-up: machine-specific details**
>
> This lecture was developed on **aarch64 (ARM64) Amazon Linux 2023**. Your
> system will likely differ — different architecture (x86_64), different OS
> (Ubuntu, macOS, Arch), different package manager (`apt`, `brew`, `pacman`).
>
> The specific download URLs, package names, and installation commands will
> change, but the **overall steps remain the same**:
>
> 1. Get a RISC-V bare-metal cross-compiler
> 2. Get QEMU with RISC-V system emulation
> 3. Get GDB for RISC-V
> 4. Verify end-to-end: compile → run on QEMU → debug with GDB
>
> If you're on a different platform, an AI coding assistant can easily adapt
> the commands for you — just describe your OS and architecture.

---

## What are we setting up and why?

Before writing a single line of kernel code, we need three tools:

| Tool | What it does | Why we need it |
|------|-------------|----------------|
| **Cross-compiler** (`riscv-none-elf-gcc`) | Translates C code into RISC-V machine code | Our host machine isn't RISC-V — we can't use the system `gcc` |
| **Emulator** (`qemu-system-riscv64`) | Pretends to be a RISC-V computer | We don't have physical RISC-V hardware |
| **Debugger** (`riscv-none-elf-gdb`) | Stops the CPU, inspects registers and memory | When your `printf` is broken, you need another way to see what's happening |

These three form a feedback loop that we'll use for the entire project:

```
Write code → Cross-compile → Run on QEMU → Debug with GDB → Repeat
```

### Cross-compiler vs native compiler

Your system has `gcc`, but it produces machine code for your host CPU (aarch64,
x86_64, etc.). We need code that runs on a RISC-V CPU. A **cross-compiler** is
a compiler that runs on one architecture but produces code for a different one.

The naming convention tells you everything:

```
riscv-none-elf-gcc
  │     │    │
  │     │    └── Output format: ELF (standard binary format)
  │     └─────── Target OS: none (bare-metal, no OS underneath)
  └───────────── Target arch: RISC-V
```

The "none" part is important. There's also `riscv64-linux-gnu-gcc`, which
targets RISC-V *running Linux*. That cross-compiler assumes a Linux kernel and
glibc are available — exactly what we *don't* have when building an OS from
scratch.

### System emulator vs userspace emulator

QEMU comes in two flavors:

- **`qemu-system-riscv64`** — emulates an entire RISC-V *computer*: CPU,
  RAM, UART, interrupt controller, disk, etc. This is what we need — our OS
  talks directly to hardware.
- **`qemu-riscv64`** — emulates only a RISC-V *CPU*, then translates
  Linux system calls to your host OS. Useful for running RISC-V Linux
  binaries, useless for OS development.


## Step 1: Installing the cross-compiler

### Finding prebuilt binaries

Building a GCC cross-compiler from source takes 30-40 minutes even on a fast
machine. Before committing to that, we looked for prebuilt binaries.

**Where we searched:**

| Source | Result | Notes |
|--------|--------|-------|
| [riscv-collab/riscv-gnu-toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain/releases) (official upstream) | x86_64 only | No aarch64 builds |
| Amazon Linux `dnf` repos | Not available | Only has native GCC |
| **[xPack](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases)** | **Available for linux-arm64** | Multi-platform prebuilt toolchains |

The [xPack project](https://xpack-dev-tools.github.io/) specifically packages
GNU toolchains for multiple platforms (x86_64, aarch64, macOS arm64). If you're
on a less common host architecture, xPack is often the first place to check.

**Lesson learned:** the official upstream repo doesn't always provide binaries
for your platform. Don't stop at one source — check xPack, Bootlin, your distro
repos, and Homebrew/Nix before resorting to building from source.

### Verifying the toolchain is suitable

Before downloading 406 MB, we verified the xPack toolchain meets our needs:

1. **Does it support RV64?** Yes — includes `rv64imac/lp64` multilib (64-bit
   base + multiply + atomics + compressed instructions). This matches QEMU's
   `virt` machine.
2. **Does it include all required tools?** Yes — gcc, ld, as, objcopy,
   objdump, readelf, nm, and GDB.
3. **Does it run on our system?** Requires glibc ≥ 2.28. We have 2.34. Good.

### Installation

```bash
# Download
curl -sL -o /tmp/riscv-toolchain.tar.gz \
  "https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v15.2.0-1/xpack-riscv-none-elf-gcc-15.2.0-1-linux-arm64.tar.gz"

# Extract to /opt/riscv
sudo mkdir -p /opt/riscv
sudo tar xzf /tmp/riscv-toolchain.tar.gz -C /opt/riscv --strip-components=1

# Add to PATH (put this in ~/.bashrc for persistence)
export PATH="/opt/riscv/bin:$PATH"

# Verify
riscv-none-elf-gcc --version
```

**Why `/opt/riscv`?** It's the de facto standard for manually installed
cross-compilers. Most RISC-V tutorials assume this path. It's also cleanly
isolated — `rm -rf /opt/riscv` removes everything.

### Tool prefix difference

The xPack toolchain uses `riscv-none-elf-` as the prefix, while many tutorials
reference `riscv64-unknown-elf-`. They are functionally identical:

| xPack naming | Upstream naming | Same tool? |
|-------------|----------------|------------|
| `riscv-none-elf-gcc` | `riscv64-unknown-elf-gcc` | Yes |
| `riscv-none-elf-ld` | `riscv64-unknown-elf-ld` | Yes |
| `riscv-none-elf-gdb` | `riscv64-unknown-elf-gdb` | Yes |

The xPack version uses `none` (no OS) instead of `unknown` (unspecified
vendor), and drops the `64` because the toolchain is multi-target (supports
both rv32 and rv64 via multilib). Just use the correct prefix in your Makefile
and everything works the same.

> **Where did this compiler come from? (The bootstrapping problem)**
>
> We just installed a cross-compiler — a program that runs on ARM64 but
> produces RISC-V code. That compiler was itself compiled by *another*
> compiler. Which was compiled by another. How does this chain start?
>
> It goes all the way back to the 1970s. The very first C compiler
> (Dennis Ritchie, Bell Labs) was written in **B**, an earlier language.
> The B compiler was written in **assembly**. The assembler was written
> in **machine code**. And machine code is just numbers — entered by
> hand via front-panel switches on a PDP-7.
>
> ```
> Hand-toggled machine code
>   → assembler (written in machine code)
>     → B compiler (written in assembly)
>       → first C compiler (written in B)
>         → C compiler rewritten in C, compiled by the B-based one
>           → now it compiles itself ← "self-hosting"
> ```
>
> That last step is the key. Once a C compiler can **compile its own
> source code**, it's self-sustaining. You no longer need the earlier
> tools. This is called **bootstrapping** — the compiler pulls itself up
> by its own bootstraps.
>
> Every compiler since then inherits from this chain. When GCC was
> created (Richard Stallman, 1987), he compiled it with an existing Unix
> C compiler. When Clang/LLVM was created, they compiled it with GCC.
> Nobody starts from zero — you always borrow an existing compiler to
> build the next one.
>
> **Cross-compilation** adds one more twist. Our `riscv-none-elf-gcc`
> was built like this:
>
> 1. Start with a working GCC on ARM64 (compiles ARM64 → ARM64)
> 2. Use it to compile GCC's source code, configured with
>    `--target=riscv-none-elf`
> 3. The result is an ARM64 binary that *outputs* RISC-V instructions
>
> The cross-compiler is just a normal ARM64 program. It happens to emit
> RISC-V code instead of ARM64 code because the **target architecture**
> is a build-time configuration option in GCC — the compilation and
> optimization logic is the same, only the final "emit instructions"
> backend differs.
>
> Nobody wrote a RISC-V compiler from scratch. GCC has been a
> **retargetable** compiler since its inception — adding RISC-V support
> meant writing a new backend ("here's how to turn GCC's internal
> representation into RISC-V instructions"), contributed primarily by
> engineers at SiFive (a RISC-V chip company) around 2017-2018. The
> frontend (parsing C), middle (optimizations), and infrastructure
> (40+ years of work) were already there.


## Step 2: Installing QEMU

### Same search process

| Source | Result |
|--------|--------|
| Amazon Linux `dnf` repos | Only `qemu-img` (disk utility, not the emulator) |
| QEMU GitHub releases | No prebuilt binaries at all (expects distros to package it) |
| **[xPack](https://github.com/xpack-dev-tools/qemu-riscv-xpack/releases)** | **Available for linux-arm64** |

Before downloading, we verified the package includes `qemu-system-riscv64`
(the system emulator, not just the userspace emulator) by peeking inside the
tarball:

```bash
curl -sL "<url>" | tar tz | grep 'bin/qemu'
# Output:
# xpack-qemu-riscv-9.2.4-1/bin/qemu-system-riscv64   ← this is what we need
# xpack-qemu-riscv-9.2.4-1/bin/qemu-system-riscv32
```

### Installation

```bash
curl -sL -o /tmp/qemu-riscv.tar.gz \
  "https://github.com/xpack-dev-tools/qemu-riscv-xpack/releases/download/v9.2.4-1/xpack-qemu-riscv-9.2.4-1-linux-arm64.tar.gz"

sudo mkdir -p /opt/qemu-riscv
sudo tar xzf /tmp/qemu-riscv.tar.gz -C /opt/qemu-riscv --strip-components=1

export PATH="/opt/qemu-riscv/bin:$PATH"

# Verify — check that the "virt" machine is supported
qemu-system-riscv64 -machine help | grep virt
# Output: virt    RISC-V VirtIO board
```

### Why the "virt" machine?

QEMU can emulate many different board designs. The `virt` machine is a
synthetic platform that doesn't correspond to any real hardware — it's
designed specifically for software development, with a clean set of
well-documented virtual devices:

- **16550 UART** at `0x10000000` — serial console (our "Hello" goes here)
- **CLINT** at `0x2000000` — core-local interrupt controller (timer)
- **PLIC** at `0xC000000` — platform-level interrupt controller
- **virtio** — virtual block device, network, etc.

We'll interact with each of these as the project progresses.


## Step 3: Bare-metal "Hello World"

With the toolchain and emulator installed, we can now prove they work together
by writing the smallest possible program that produces visible output.

### The big picture — what happens from power-on to "Hello"

```
QEMU starts
    ↓
QEMU firmware jumps to 0x80000000    ← hardcoded by QEMU's "virt" machine spec
    ↓
_start runs  (entry.S)               ← sets up stack, calls main()
    ↓
main() runs  (main.c)                ← writes bytes to UART memory address
    ↓
UART hardware transmits bytes        ← QEMU shows them in your terminal
```

No OS. No syscalls. No C runtime. Just you and the hardware.

> **How does the ELF binary get into memory?**
>
> You might wonder: if there's no OS on the target, who loads our
> program? The answer depends on the environment:
>
> **On QEMU:** QEMU itself loads it. QEMU is a normal Linux process on
> your host machine. When you pass `-kernel program.elf`, QEMU opens
> the file with regular host OS syscalls, parses the ELF headers to
> find where each section goes, copies the contents into the emulated
> machine's RAM (which is just a `malloc`'d buffer in the QEMU
> process), sets the emulated CPU's program counter to `0x80000000`,
> and starts executing. "Loading at address `0x80000000`" just means
> writing bytes to an offset in that buffer.
>
> **On real hardware,** a chain of increasingly capable software does
> the loading, each stage handing off to the next:
>
> ```
> Power on
>   │  CPU starts at a hardwired address (set by silicon design)
>   ▼
> ROM firmware (burned into chip at factory)
>   │  Tiny code that knows how to read from flash/SD card
>   │  Loads the bootloader into RAM
>   ▼
> Bootloader (U-Boot / OpenSBI)
>   │  More capable — understands filesystems, ELF format
>   │  Loads the kernel into RAM at the right address
>   │  Sets up minimal hardware (DRAM controller, clocks)
>   ▼
> Your OS kernel
>   │  Takes over from here
>   ▼
> ```
>
> Each stage is effectively "the OS" for the next stage. The ROM
> firmware is the ultimate answer to "but who loads the loader?" — it's
> not loaded at all, it's physically part of the hardware. ROM
> (Read-Only Memory) is a small chip (or a dedicated region on the same
> silicon die) whose contents are determined at manufacturing time —
> the bits are baked into the silicon or flashed once into a one-time-
> programmable memory. The CPU sees ROM as just another address range
> on the memory bus, no different from RAM from its perspective. It
> fetches instructions from address `0x1000`, and the memory bus routes
> that read to the ROM chip instead of DRAM. A typical boot ROM is
> tiny — 32 to 128KB — just enough code to initialize the hardware and
> load the next stage from flash or SD card.
>
> ```
> ┌─────────────────────────────────────┐
> │         SoC (System on Chip)        │
> │                                     │
> │   ┌───────┐  ┌───────┐  ┌───────┐   │
> │   │  CPU  │  │  ROM  │  │  RAM  │   │
> │   │ cores │  │ 64KB  │  │       │   │
> │   └───┬───┘  └───┬───┘  └───┬───┘   │
> │       └──────────┴──────────┘       │
> │             memory bus              │
> └─────────────────────────────────────┘
> ```
>
> On QEMU, this entire chain is simulated. QEMU's internal firmware
> lives at address `0x1000` and does minimal setup before jumping to
> `0x80000000`. That's why GDB shows `pc = 0x1000` at startup (Step 4
> below) — that's QEMU's built-in firmware running, before it reaches
> our `_start`. The `-bios none` flag tells QEMU to skip this firmware
> and jump directly to our code, but behind the scenes QEMU still does
> the ELF loading itself.
>
> We'll revisit the boot sequence in more detail in Phase 1, where
> we'll deal with RISC-V privilege modes (M-mode → S-mode) and what
> OpenSBI does for us. For now, just know: something always puts your
> code in memory before `_start` runs — on QEMU, it's the emulator
> itself.

### What we wrote

Four files — see the source code in [bare-metal-hello/](bare-metal-hello/)
with inline comments explaining every line. For build/run/debug instructions,
see [bare-metal-hello/GUIDE.md](bare-metal-hello/GUIDE.md).

Key compiler flags used in the Makefile:

| Flag | Purpose |
|------|---------|
| `-march=rv64imac` | Target the RV64 instruction set with multiply, atomics, compressed |
| `-mabi=lp64` | 64-bit longs and pointers, no hardware floating point |
| `-ffreestanding` | Don't assume a hosted C environment (no libc) |
| `-nostdlib` | Don't link the standard library or startup files |
| `-mcmodel=medany` | Allow code at any address (not just near 0) |
| `-g` | Include debug symbols (so GDB can show source lines) |


### The linker script — deep dive

The linker script ([linker.ld](bare-metal-hello/linker.ld)) does two
fundamental jobs:

**Job 1 — Set the entry point:**
```
ENTRY(_start)
```
This writes `_start`'s address into the ELF `e_entry` field. QEMU reads this
ELF header and knows where to jump.

**Job 2 — Control memory layout:**

The `. = 0x80000000` line sets the **location counter** — the linker's cursor
that tracks "where am I placing things right now."

```
. = 0x80000000;     ← cursor starts here

.text : {
    *(.text.entry)   ← place ALL .text.entry sections from ALL .o files first
    *(.text .text.*)  ← then all normal code
}
```

The `*(.text.entry)` glob is the key trick. By placing `.text.entry` first,
the linker guarantees `_start` lands at exactly `0x80000000` — the address
QEMU jumps to. If you used just `*(.text)`, the order would be undefined and
`_start` might not be first.

**The stack is just reserved address space:**
```
. = ALIGN(16);
. += 4096;          ← advance location counter by 4KB (no actual section)
stack_top = .;      ← bind the symbol "stack_top" to the current address
```

There's no `.stack` section — the linker just skips the cursor forward by
4096 bytes, leaving that address range reserved. `stack_top` becomes a symbol
whose value is just a number (an address). `entry.S` then loads that number
into `sp`.

Full memory layout:

```
0x80000000  ┌──────────────┐
            │  .text       │  Code (entry.S first, then main.c)
            ├──────────────┤
            │  .rodata     │  Read-only data (string literals)
            ├──────────────┤
            │  .data       │  Initialized global variables
            ├──────────────┤
            │  .bss        │  Uninitialized globals (zeroed)
            ├──────────────┤
            │  (4KB stack) │  Stack space
            ├──────────────┤
            │  stack_top   │  sp starts here (grows down)
            └──────────────┘
```


### entry.S — deep dive

Source: [entry.S](bare-metal-hello/entry.S)

**Why assembly and not C?**

Because C **requires** a valid stack to work. Even a function call like
`call main` needs the stack for the return address. You can't set up the stack
*in C* because you need a stack to run C in the first place. It's a
chicken-and-egg problem — assembly breaks the cycle.

**`la sp, stack_top` — how it works:**

`la` = **load address**. It's a pseudo-instruction that the assembler expands
to two real instructions:

```asm
auipc sp, %hi(stack_top)     ← load upper 20 bits of address, PC-relative
addi  sp, sp, %lo(stack_top)  ← add lower 12 bits
```

The assembler/linker fills in the actual value of `stack_top` (from the linker
script). After this, `sp` holds the address where `stack_top` ended up.

**Stack grows downward — why?**

This is a CPU convention (not just RISC-V — ARM, x86, and most architectures
do this). When you push something onto the stack:

```
sp = sp - 8         ← sp moves toward lower addresses
memory[sp] = value
```

So `stack_top` is the *highest* address in the stack region, and the stack
expands downward toward lower addresses. If it grows too far, it runs into
`.bss` or `.data` — a stack overflow. In bare metal there's no guard page,
so it would silently corrupt your data.

```
0x80001000  stack_top  ← sp starts here
0x80000FF8             ← after first push (sp -= 8)
0x80000FF0             ← after second push
   ...
0x80000000             ← danger zone: hits your code!
```

**`wfi` — Wait For Interrupt:**

If `main()` returns, there's nowhere to go. The CPU has no OS to return to —
it would start executing whatever garbage bytes come after your binary in
memory. So we spin forever with `wfi`, which halts the CPU core in a low-power
state until an interrupt wakes it. Since we have no interrupts configured, it
sleeps forever.


### main.c — deep dive

Source: [main.c](bare-metal-hello/main.c)

**Memory-mapped I/O — the core concept:**

Normal memory: CPU writes a value → DRAM stores it → CPU reads it back later.

Memory-mapped I/O: CPU writes a value → **the hardware device intercepts the
bus transaction and does something** → nothing is actually stored in RAM.

The CPU doesn't know the difference. From its perspective, it's just a store
instruction to an address. The memory bus routes it to either DRAM or a device
based on the address range:

```
Address map on QEMU "virt":
0x00000000 - 0x0FFFFFFF   → various peripherals (ROM, CLINT, PLIC...)
0x10000000                 → UART0   ← writing here = sending a character
0x80000000 - ...           → DRAM    ← your code lives here
```

> **What is UART?**
>
> UART stands for **Universal Asynchronous Receiver/Transmitter**. It's
> one of the simplest hardware communication interfaces — essentially a
> serial port.
>
> In a physical setup, UART connects two devices with just two wires:
> one for sending (TX) and one for receiving (RX). Data is sent one bit
> at a time at a pre-agreed speed (the "baud rate," e.g., 115200 bits
> per second). There's no shared clock signal between the two sides —
> that's the "asynchronous" part. Each side just trusts the other to
> send bits at the agreed rate.
>
> ```
> ┌────────────┐    TX ──────→ RX    ┌────────────┐
> │  Device A  │                     │  Device B  │
> │  (CPU)     │    RX ←────── TX    │ (Terminal) │
> └────────────┘                     └────────────┘
> ```
>
> The **16550 UART** is a specific chip design (originally from the
> 1980s) that became the de facto standard for serial ports in PCs. It
> exposes a handful of 8-bit **registers** at consecutive memory
> addresses. The most important ones:
>
> | Register | Offset | What it does |
> |----------|--------|-------------|
> | THR (Transmit Holding Register) | +0 | Write a byte here → it gets sent out the TX wire |
> | RHR (Receive Holding Register) | +0 | Read a byte here ← it arrived from the RX wire |
> | LSR (Line Status Register) | +5 | Status bits: "TX ready for next byte", "RX has data", etc. |
>
> THR and RHR share the same address — a write goes to THR (transmit),
> a read goes to RHR (receive). The hardware knows which you meant by
> whether the CPU performed a store or a load.
>
> On QEMU's `virt` machine, the 16550 UART is mapped at address
> `0x10000000`. When we write a byte to that address, QEMU's emulated
> UART "transmits" it — which in practice means printing it to your
> terminal. When we read from offset +5 (the LSR at `0x10000005`), we
> can check if the device is ready for the next byte.
>
> In this lecture, we simplify by just writing to `0x10000000` without
> checking the status register. This works on QEMU because the emulated
> UART is always ready instantly. On real hardware, you'd check LSR
> first to avoid sending data faster than the wire can carry it. We'll
> build a proper UART driver with status checking and interrupt support
> in Phase 1.
>
> The key takeaway: UART is just a few hardware registers at known
> memory addresses. "Writing to the serial console" means storing a byte
> to an address. That's it — no protocol stack, no driver framework,
> just a store instruction.

**The `volatile` keyword — critical:**

Without `volatile`:
```c
// Compiler sees: you're writing to the same address every loop iteration
// Compiler thinks: "redundant writes, I'll optimize to just the last one"
// Result: only the last character is sent, or none at all
*uart = 'H';
*uart = 'e';
*uart = 'l';   // compiler might keep only this
```

With `volatile`:
```c
volatile char *uart = (volatile char *)0x10000000;
// Compiler must emit a store instruction for EVERY write, in order
// Because volatile says: "this has side effects you don't understand"
```

`volatile` also prevents **reordering** — without it, the compiler might
rearrange your writes. For hardware registers, order always matters.

**Why no `printf`?**

`printf` ultimately calls `write()`, which is a Linux **syscall**. A syscall
traps into the kernel and asks the OS to do something. Here, *you are* the
kernel — there's no OS below you to call. You have to speak directly to the
hardware. That's why we write to `0x10000000` directly.


### What's missing vs a real OS kernel

This is a complete bare-metal program, but a real OS would need more before
calling `main()`:

| Thing | Why needed |
|-------|-----------|
| Zero out `.bss` | C standard guarantees uninitialized globals are zero. QEMU may not guarantee this. You'd loop over `.bss` start→end and write zeros. |
| Copy `.data` from ROM to RAM | On real hardware (not QEMU), initialized globals start in flash/ROM and must be copied to RAM at boot. |
| Set up trap/interrupt vector | So the CPU knows where to jump on exceptions/interrupts instead of going to a random address. |
| Multi-core parking | On real RISC-V boards, all cores start at `_start`. You must check the hart (core) ID and park all cores except core 0. |
| Virtual memory (MMU setup) | For isolation between processes. Not needed for bare metal but essential for a real OS. |

We legitimately skip all of this because QEMU's `virt` machine provides a
clean simulated environment — zeroed memory, single core startup by default,
no ROM-to-RAM copy needed. On real hardware, the startup path would be messier.
We'll build these pieces as the project progresses.


## Step 4: Debugging with GDB

GDB + QEMU together give you the same debugging experience as running code on
real hardware with a JTAG debugger. This is how kernel developers work.

### How it works

```
┌──────────────┐        TCP :1234        ┌──────────────┐
│     GDB      │ ←── GDB protocol ───→   │     QEMU     │
│              │                         │              │
│ reads .elf   │  "set breakpoint at     │ runs RISC-V  │
│ (symbols,    │   0x80000034"           │ CPU, stops   │
│  source)     │  "read register pc"     │ when told    │
│              │  "continue"             │              │
└──────────────┘                         └──────────────┘
```

QEMU runs the CPU. GDB connects over TCP and controls it: pause, resume, set
breakpoints, read registers. GDB also loads the `.elf` file locally to map
addresses back to source code and function names.

### What we observed

**At startup (before our code runs):**
```
pc = 0x1000    ← QEMU's internal reset vector (firmware)
sp = 0x0       ← no stack configured yet
```

QEMU starts at its own firmware code at `0x1000`, which then jumps to our
kernel at `0x80000000`. We don't see this — we set a breakpoint on `main` and
skip past it.

**At `main()`:**
```
pc = 0x80000034   ← our main() function
sp = 0x80001080   ← stack pointer (set by entry.S)
ra = 0x8000000c   ← return address → spin loop in entry.S
```

Things to notice:
- `pc` is inside our code at `0x80000xxx` — the linker script worked.
- `sp` is non-zero and points to our stack region — `entry.S` worked.
- `ra` (return address) points back to the `spin` label in `entry.S`. This is
  where `main()` would return to if it ever returns (it shouldn't, but the
  spin loop catches it safely).

**Disassembly of `main()`:**

The compiler (with `-O2`) inlined `uart_puts` and `uart_putc` into a tight
loop:

```asm
main:
  auipc  a5, 0x0          # Load address of the string
  addi   a5, a5, 44
  li     a4, 72            # 72 = ASCII 'H' (first character)
  lui    a3, 0x10000       # a3 = 0x10000000 (UART address)
loop:
  sb     a4, 0(a3)         # Write character to UART
  addi   a5, a5, 1         # Advance string pointer
  lbu    a4, 0(a5)         # Load next character
  bne    a5, a2, loop      # Loop until end of string
  ret
```

The entire "print a string" operation is just 5 instructions in a loop: load a
byte, store it to the UART address, advance, repeat. This is what bare-metal
programming looks like — no layers of abstraction, just direct hardware
interaction.

### Practical GDB usage

For detailed instructions on running GDB with QEMU (commands, common pitfalls,
port conflicts, batch mode), see [bare-metal-hello/GUIDE.md](bare-metal-hello/GUIDE.md).


## Summary

What we accomplished:

| Item | Tool | Location |
|------|------|----------|
| Cross-compiler | `riscv-none-elf-gcc` 15.2.0 (xPack) | `/opt/riscv/bin/` |
| Emulator | `qemu-system-riscv64` 9.2.4 (xPack) | `/opt/qemu-riscv/bin/` |
| Debugger | `riscv-none-elf-gdb` 16.3 (bundled with compiler) | `/opt/riscv/bin/` |
| Proof of life | "Hello from bobchouOS!" via bare-metal UART output | `bare-metal-hello/` |

The complete bare-metal hello world exercise (source code, build/run/debug
guide) is in [bare-metal-hello/](bare-metal-hello/).

### What to pay attention to going forward

- **Memory-mapped I/O** — hardware devices appear as memory addresses. We'll
  see this pattern repeatedly: UART, timer, interrupt controller, virtio.
- **The linker script** — it controls where code lives in memory. The kernel
  linker script in Phase 1 will be more sophisticated but follows the same
  principles.
- **GDB + QEMU** — this is your primary debugging tool for the rest of the
  project. Get comfortable with breakpoints, `stepi`, and `info registers`.
- **C without an OS** — no `malloc`, no `printf`, no `exit`. Everything must
  be built from scratch. Phase 0-2 (C recap) will prepare you for this.
