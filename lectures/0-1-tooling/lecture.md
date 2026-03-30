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

### The challenge

There is no OS running. No C runtime calls `main()` for us. No `printf()`
exists. We have to:

1. Set up a stack (C functions need it for local variables and calls)
2. Jump to our C code
3. Write characters directly to hardware

### What we wrote

Four files — see the code in [bare-metal-hello/](bare-metal-hello/) with
inline comments explaining every line:

**[entry.S](bare-metal-hello/entry.S)** — Assembly entry point (first
instruction executed):

```
QEMU jumps to 0x80000000 → _start runs → sets stack pointer → calls main()
```

The stack pointer must be set before any C code runs. On RISC-V, the stack
grows downward, so we point `sp` to the *top* of a 4KB stack region.

**[main.c](bare-metal-hello/main.c)** — C code that writes to the UART:

```c
#define UART0_ADDR 0x10000000

void uart_putc(char c) {
    volatile char *uart = (volatile char *)UART0_ADDR;
    *uart = c;
}
```

This is **memory-mapped I/O**: the UART's transmit register is mapped to
memory address `0x10000000`. Writing a byte there doesn't store it in RAM —
it sends it out the serial port. QEMU then displays it on your terminal.

The `volatile` keyword is critical: it tells the compiler "this memory
location has side effects — don't optimize away repeated writes to it." Without
`volatile`, the compiler might see multiple writes to the same address and
decide only the last one matters, eating our entire string.

**[linker.ld](bare-metal-hello/linker.ld)** — Memory layout:

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

The linker script ensures `.text.entry` (containing `_start`) is placed first
at `0x80000000` — exactly where QEMU expects the kernel.

**[Makefile](bare-metal-hello/Makefile)** — Build and run commands. Key flags:

| Flag | Purpose |
|------|---------|
| `-march=rv64imac` | Target the RV64 instruction set with multiply, atomics, compressed |
| `-mabi=lp64` | 64-bit longs and pointers, no hardware floating point |
| `-ffreestanding` | Don't assume a hosted C environment (no libc) |
| `-nostdlib` | Don't link the standard library or startup files |
| `-mcmodel=medany` | Allow code at any address (not just near 0) |
| `-g` | Include debug symbols (so GDB can show source lines) |

### Build bugs we hit

We encountered three build errors. These are common pitfalls in bare-metal
development and worth understanding:

#### Bug 1: Linker script syntax — `ALIGN` vs `. = ALIGN`

```
riscv-none-elf-ld: cannot find ALIGN: No such file or directory
```

Inside a linker script section, bare `ALIGN(16);` is not a directive — the
linker interprets it as a filename. The correct syntax is `. = ALIGN(16);`,
which means "advance the location counter to the next 16-byte boundary."

The `.` (dot) in linker scripts is the **location counter** — it tracks where
the linker is currently placing data. This is a concept unique to linker
scripts and doesn't appear in C.

#### Bug 2: 32-bit vs 64-bit ABI mismatch

```
riscv-none-elf-ld: entry.o: ABI is incompatible with that of the selected emulation:
  target emulation `elf64-littleriscv' does not match `elf32-littleriscv'
```

The xPack linker is a multi-target binary that **defaults to 32-bit RISC-V**.
Even though we compiled with `-march=rv64imac`, the linker doesn't infer the
target from the object files.

Fix: pass `-m elf64lriscv` to the linker explicitly. This is specific to
multi-target toolchains like xPack; the upstream `riscv64-unknown-elf-ld`
defaults to 64-bit.

This kind of bug is a good example of why the tooling step exists —
discovering these quirks now, with a trivial program, is much easier than
discovering them while debugging a complex kernel.

#### Bug 3: RWX segment warning

```
riscv-none-elf-ld: warning: hello.elf has a LOAD segment with RWX permissions
```

Our simple linker script puts everything in one segment that is readable,
writable, and executable. A proper kernel should separate these (code =
read+execute, data = read+write) for security. We'll fix this in Phase 1.

### Running it

```bash
make run
# Output: Hello from bobchouOS!
# Press Ctrl-A then X to exit QEMU
```


## Step 4: Debugging with GDB

GDB + QEMU together give you the same debugging experience as running code on
real hardware with a JTAG debugger. This is how kernel developers work.

### How it works

```
┌──────────────┐         TCP :1234         ┌──────────────┐
│     GDB      │  ←─── GDB protocol ───→   │     QEMU     │
│              │                            │              │
│ reads .elf   │   "set breakpoint at       │ runs RISC-V  │
│ (symbols,    │    0x80000034"             │ CPU, stops   │
│  source)     │   "read register pc"       │ when told    │
│              │   "continue"               │              │
└──────────────┘                            └──────────────┘
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

### GDB pitfalls we hit

**Port conflict:** if a previous QEMU is still running, port 1234 is taken:
```
qemu-system-riscv64: -s: Failed to find an available port: Address already in use
```
Fix: `kill $(lsof -ti:1234)` or `pkill -f qemu-system-riscv`

**ELF loading in batch mode:** xPack GDB 16.3 silently ignores the positional
file argument in `--batch` mode. Use `-ex "file hello.elf"` instead. See
[bare-metal-hello/GUIDE.md](bare-metal-hello/GUIDE.md) for full details.


## Summary

What we accomplished:

| Item | Tool | Location |
|------|------|----------|
| Cross-compiler | `riscv-none-elf-gcc` 15.2.0 (xPack) | `/opt/riscv/bin/` |
| Emulator | `qemu-system-riscv64` 9.2.4 (xPack) | `/opt/qemu-riscv/bin/` |
| Debugger | `riscv-none-elf-gdb` 16.3 (bundled with compiler) | `/opt/riscv/bin/` |
| Proof of life | "Hello from bobchouOS!" via bare-metal UART output | `bare-metal-hello/` |

The complete bare-metal hello world exercise (with source code and a detailed
run guide) is in [bare-metal-hello/](bare-metal-hello/).

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
