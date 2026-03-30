# Bare-Metal Hello World — Practical Guide

This guide walks through building, running, and debugging the bare-metal hello
world program on QEMU. It covers the exact issues we hit and how we solved them.

## Prerequisites

These tools must be on your `PATH`:

| Tool | Purpose | Verify with |
|------|---------|-------------|
| `riscv-none-elf-gcc` | Cross-compiler (C → RISC-V machine code) | `riscv-none-elf-gcc --version` |
| `riscv-none-elf-ld` | Linker (combines .o files into an ELF) | `riscv-none-elf-ld --version` |
| `riscv-none-elf-gdb` | Debugger | `riscv-none-elf-gdb --version` |
| `qemu-system-riscv64` | RISC-V system emulator | `qemu-system-riscv64 --version` |

If installed to `/opt/riscv` and `/opt/qemu-riscv`:

```bash
export PATH="/opt/riscv/bin:/opt/qemu-riscv/bin:$PATH"
```

## Build

```bash
make
```

This compiles `entry.S` and `main.c` into RISC-V object files, then links them
into `hello.elf` using the memory layout defined in `linker.ld`.

### Build errors we encountered

#### 1. Linker script: `ALIGN` vs `. = ALIGN`

**Error:**
```
riscv-none-elf-ld: cannot find ALIGN: No such file or directory
```

**Cause:** Inside a linker section, bare `ALIGN(16);` is invalid syntax. The
linker interprets `ALIGN` as a filename to link against.

**Fix:** Use `. = ALIGN(16);` — this assigns the current location counter to
the next 16-byte aligned address, which is the correct way to align within a
section.

```
# Wrong
.bss : {
    ALIGN(16);
    *(.bss .bss.*)
}

# Correct
.bss : {
    . = ALIGN(16);
    *(.bss .bss.*)
}
```

#### 2. Linker ABI mismatch: 32-bit vs 64-bit

**Error:**
```
riscv-none-elf-ld: entry.o: ABI is incompatible with that of the selected emulation:
  target emulation `elf64-littleriscv' does not match `elf32-littleriscv'
```

**Cause:** The xPack `riscv-none-elf-ld` is a multi-target linker that
defaults to 32-bit RISC-V emulation. Even though we compiled with
`-march=rv64imac` (64-bit), the linker doesn't infer the target from the
object files — it uses its built-in default.

This is specific to the xPack toolchain. The upstream `riscv64-unknown-elf-ld`
defaults to 64-bit because it's built for a single target.

**Fix:** Pass `-m elf64lriscv` to the linker to explicitly select 64-bit
little-endian RISC-V:

```makefile
$(LD) -m elf64lriscv -T linker.ld -o $@ $(OBJS)
```

You can list all available emulations with `riscv-none-elf-ld -V`.

#### 3. RWX segment warning

**Warning:**
```
riscv-none-elf-ld: warning: hello.elf has a LOAD segment with RWX permissions
```

**Cause:** Our linker script doesn't separate read/write/execute permissions
for different sections. The linker merges everything into one segment that is
readable, writable, and executable.

**Impact:** Harmless for this exercise. In a real kernel, you'd want `.text` to
be read+execute only and `.data`/`.bss` to be read+write only, as a security
measure. We'll do this properly in Phase 1 when we write the real kernel linker
script.


## Run

```bash
make run
```

This launches QEMU with these flags:

| Flag | Meaning |
|------|---------|
| `-machine virt` | Use the "virt" platform (provides UART, timer, etc.) |
| `-nographic` | No GUI window; serial I/O goes to your terminal |
| `-bios none` | Don't load OpenSBI firmware; jump directly to our code |
| `-kernel hello.elf` | Load our ELF and jump to its entry point |

You should see:
```
Hello from bobchouOS!
```

**To exit QEMU:** press `Ctrl-A` then `X` (not Ctrl-C).


## Debug with GDB

This is the most involved part. GDB lets you freeze the CPU, set breakpoints,
step through code, and inspect registers and memory.

### Start QEMU in debug mode

In one terminal:
```bash
make debug
```

This adds two flags:
- `-s` — start a GDB server on `localhost:1234`
- `-S` — freeze the CPU at startup (won't execute until GDB says "continue")

QEMU will appear to hang — that's correct. It's waiting for GDB.

### Connect GDB

In a second terminal:
```bash
riscv-none-elf-gdb
```

Then inside GDB:
```gdb
(gdb) file hello.elf
(gdb) target remote localhost:1234
```

### GDB quirk: loading the ELF file

Normally you'd pass the ELF as a command-line argument:
```bash
riscv-none-elf-gdb hello.elf        # may NOT work
```

We found that with xPack GDB 16.3 in `--batch` mode, the positional argument
is silently ignored. Symptoms:
- `No symbol table is loaded`
- `No executable has been specified`
- Breakpoints on function names fail

**Reliable alternative:** use the explicit `file` command:
```bash
riscv-none-elf-gdb --batch \
  -ex "file hello.elf" \
  -ex "target remote localhost:1234" \
  -ex "break main" \
  -ex "continue" \
  ...
```

In interactive mode (without `--batch`), both methods should work.

### Common GDB commands for kernel debugging

```gdb
# Execution control
break main              # Set breakpoint on a function
break *0x80000000       # Set breakpoint on a raw address
continue                # Resume execution (run until next breakpoint)
step                    # Step one source line (enters functions)
next                    # Step one source line (skips over function calls)
stepi                   # Step one assembly instruction

# Inspection
info registers          # Show all registers
info registers pc sp ra # Show specific registers
list                    # Show source code around current position
disassemble main        # Show assembly for a function
print *uart             # Print a variable's value
x/10xb 0x10000000      # Examine 10 bytes in hex at an address
x/4i $pc               # Examine 4 instructions at current PC

# Info
backtrace               # Show call stack
info breakpoints        # List all breakpoints
```

### What to expect at startup

When GDB first connects, the CPU is at QEMU's reset vector, not your code:

```
pc = 0x1000     ← QEMU's internal firmware entry, not our code
sp = 0x0        ← no stack yet
```

After `continue` to `main`:

```
pc = 0x80000034 ← our main() function
sp = 0x80001080 ← stack pointer, set by entry.S
ra = 0x8000000c ← return address points to spin loop in entry.S
```

### Port conflict: "Address already in use"

**Error:**
```
qemu-system-riscv64: -s: Failed to find an available port: Address already in use
```

**Cause:** A previous QEMU instance is still running and holding port 1234.

**Fix:**
```bash
# Find and kill the old QEMU
kill $(lsof -ti:1234)
# Or find it by name
pkill -f qemu-system-riscv
```

Then retry `make debug`.

### Batch mode GDB session (non-interactive)

For scripted/automated debugging (useful for testing), you can run GDB
in `--batch` mode with a sequence of `-ex` commands:

```bash
ELF=hello.elf

# Start QEMU in background
qemu-system-riscv64 -machine virt -nographic -bios none \
  -kernel $ELF -s -S &
QEMU_PID=$!
sleep 1

# Run GDB commands
riscv-none-elf-gdb --batch \
  -ex "file $ELF" \
  -ex "target remote localhost:1234" \
  -ex "break main" \
  -ex "continue" \
  -ex "info registers pc sp ra" \
  -ex "disassemble main" \
  -ex "quit"

# Clean up
kill $QEMU_PID
```


## File overview

| File | What it does |
|------|-------------|
| `entry.S` | Assembly entry point: sets up stack pointer, calls `main()` |
| `main.c` | Writes "Hello from bobchouOS!" to UART at `0x10000000` |
| `linker.ld` | Places `_start` at `0x80000000` (where QEMU loads the kernel) |
| `Makefile` | Build rules + QEMU launch commands |
