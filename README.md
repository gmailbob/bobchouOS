# bobchouOS

A tiny operating system built from scratch on RISC-V, for the purpose of learning OS internals through hands-on development.

## Motivation

Operating system courses teach principles — scheduling algorithms, page tables, file systems — but rarely offer the chance to build one end to end. bobchouOS exists to bridge that gap: a minimal, educational OS that turns textbook concepts into running code.

The goal is not to build anything production-ready. It's to deeply understand how an operating system works by implementing each layer from the ground up.

## Platform & Toolchain Choices

### Why RISC-V?

| | RISC-V | x86 |
|---|--------|-----|
| **Instruction set** | ~50 base instructions, clean and orthogonal | Thousands, accumulated over 40+ years |
| **Privilege spec** | ~100 pages, well-structured | ~5,000 pages across multiple manuals |
| **Boot sequence** | Straightforward: M-mode → S-mode → U-mode | Complex: real mode → protected mode → long mode |
| **Legacy baggage** | None | Segmentation, A20 line, BIOS/UEFI compat, etc. |
| **Learning value** | Focus on OS concepts, not CPU quirks | Much time spent fighting architecture history |

RISC-V's clean design means more time learning OS concepts and less time wrestling with ISA complexity. The privileged architecture (Machine / Supervisor / User modes) maps directly to how operating systems are structured in textbooks.

**A bit of history:** RISC-V was designed at UC Berkeley starting around 2010 by Krste Asanović and David Patterson (co-author of the classic *Computer Organization and Design* textbook). Unlike ARM or x86, RISC-V is an open ISA — anyone can implement it without licensing fees. This openness is why it has strong tooling support (GCC, LLVM, QEMU) and a thriving ecosystem despite being relatively young. The name stands for the fifth generation of RISC research projects at Berkeley.

Our primary reference, **xv6**, was originally written for x86 by MIT's PDOS group (Frans Kaashoek, Robert Morris, Nickolai Zeldovich) in 2006 as a teaching reimplementation of Unix v6 (1975) — hence the name "x86 version of v6." Around 2019, MIT ported xv6 to RISC-V for their renamed 6.S081 course, for the same reasons we chose it: too much class time was spent fighting x86 quirks (real mode → protected mode → long mode, segmentation, A20 line) instead of learning OS concepts. The original x86 version (`mit-pdos/xv6-public`) is archived; the RISC-V port (`mit-pdos/xv6-riscv`) is actively maintained and what we reference throughout this project.

### Why QEMU?

- **Zero hardware risk** — no bricking, no serial cable debugging
- **Fast iteration** — `make run` boots the kernel in under a second
- **GDB integration** — attach a debugger with `qemu-system-riscv64 -s -S`, step through kernel code instruction by instruction
- **The `virt` machine** — QEMU's RISC-V `virt` platform provides a clean, well-documented set of virtual devices (16550 UART, CLINT timer, PLIC interrupt controller, virtio block device) without real hardware variability
- **Real hardware later** — once the OS works on QEMU, porting to a physical RISC-V board (e.g., SiFive, StarFive) becomes a future stretch goal

### Why C (not Rust)?

This was a deliberate choice. Both languages are viable for OS development, but they serve different purposes at different layers of the systems stack.

**C is the better fit for this project because:**

- **xv6-riscv**, the primary reference implementation, is written in C. Reading it and writing bobchouOS in the same language creates a 1:1 mapping between reference and implementation.
- The entire RISC-V bare-metal ecosystem (OpenSBI, U-Boot, Linux) is in C. Every example, every spec snippet, every tutorial assumes C.
- C is a small language. Pointers, structs, manual memory — that's essentially it. The language stays out of the way and lets you focus on OS concepts.
- In kernel code, you're directly manipulating hardware: raw pointers for page tables, volatile MMIO access, inline assembly for CSR operations. In Rust, all of this requires `unsafe` blocks, meaning you'd pay Rust's complexity cost (ownership, lifetimes, trait bounds) while losing most of its safety guarantees — the worst of both worlds for a learning project.

**Where Rust and C each shine in the systems stack:**

```
Hardware
  |
  |-- Firmware / Bootloader ----------- C + Assembly (almost exclusively)
  |-- Kernel core --------------------- C dominates, Rust entering
  |-- Device drivers ------------------ C traditionally, Rust gaining ground
  |-- OS services / daemons ----------- C or Rust (Rust shines here)
  |
  |-- Systems tools ------------------- Rust's sweet spot (ripgrep, fd, etc.)
  |-- Databases / runtimes ------------ Rust or C/C++ (TiKV, Deno, SWC...)
  |-- Network services ---------------- Rust excels (Cloudflare, Discord)
  |
  |-- Application software ------------ Go, Java, Python, TypeScript, etc.
  |
User
```

Rust's safety guarantees get more valuable as you move up from hardware. Closer to metal, you're in `unsafe` territory anyway. Higher up, Rust prevents entire categories of bugs (use-after-free, data races, buffer overflows) that C simply cannot catch at compile time. "Rust is the future" is true for systems software — but for learning OS internals at the kernel level, C remains the most practical and well-supported choice.

## Development Plan

### Phase 0 — Tooling & C Fundamentals

- Install the RISC-V cross-compiler toolchain (`riscv-none-elf-gcc`)
- Install QEMU with RISC-V support (`qemu-system-riscv64`)
- Refresh C fundamentals: pointers, structs, manual memory management, bitwise operations, the preprocessor
- Refresh RISC-V assembly: registers, calling convention, basic instructions
- Write a bare-metal "hello world" that outputs to UART on QEMU — proving the toolchain works end to end
- Set up GDB with QEMU (`-s -S` flags) and practice stepping through code

### Phase 1 — Boot & Early Kernel

- Write a linker script defining kernel memory layout (code, data, BSS, stack sections)
- Write `entry.S` — a minimal assembly entry point that sets up the stack pointer and jumps to C
- Implement a UART driver (`uart.c`) targeting the 16550 UART on QEMU's `virt` machine
- Implement `kprintf` for formatted serial output
- Build a Makefile: `make run` compiles, links, and launches QEMU

> **Milestone:** "hello from bobchouOS" appears on the serial console.

### Phase 2 — RISC-V Privilege Levels & Traps

- Understand RISC-V privilege modes: Machine (M), Supervisor (S), User (U)
- Switch from M-mode to S-mode at boot (mstatus, mepc, mret)
- Configure PMP for S-mode access to all physical memory
- Set up the trap vector (`stvec`) and write a trap handler (assembly stub → C dispatcher)
- Handle timer interrupts via the CLINT
- Handle basic exceptions (illegal instruction, page fault, breakpoint) — print diagnostics and halt

> **Milestone:** the kernel handles timer interrupts and exceptions cleanly.

### Phase 3 — Physical Memory Management

- Detect available RAM (hardcoded for QEMU `virt`: 0x80000000–0x88000000)
- Implement a page allocator: free list of 4KB pages (`kalloc()` / `kfree()`)
- Test allocation and deallocation before building on top of it

### Phase 4 — Virtual Memory (Paging)

- Study the RISC-V Sv39 paging scheme (3-level page tables, 39-bit virtual addresses)
- Implement page table creation, page mapping, and page table walks (`vm.c`)
- Set up kernel page tables: identity-map kernel code/data, map UART, CLINT, PLIC
- Enable paging via the `satp` CSR
- Implement a kernel heap allocator (`kmalloc` / `kfree`) on top of the page allocator

> **Milestone:** paging is enabled, kernel runs with virtual memory, dynamic allocation works.

### Phase 5 — Processes & Scheduling

- Implement intrusive data structures for process management (doubly-linked list, hash table)
- Define `struct proc` — PID, state, kernel stack, context, parent-child relationships
- Implement `swtch()` — assembly routine to save/restore callee-saved registers for context switching
- Implement a round-robin scheduler driven by timer interrupts (on-demand one-shot via mini-SBI)
- Run multiple kernel threads with preemptive interleaving on a single hart
- Implement spinlocks (amoswap, memory fences, irqsave pattern) and wait queues (targeted sleep/wakeup)
- Implement `exit()`, `wait()`, and `kill()` — full process lifecycle with zombie reaping

> **Milestone:** multiple processes running concurrently with preemptive scheduling; full process lifecycle (create, run, exit, reap).

### Phase 6 — User Mode & System Calls

- Set up per-process user-mode page tables (separate address spaces)
- Implement trampoline page and trapframe for user↔kernel transitions
- Transition processes from S-mode to U-mode via `sret`
- Implement the `ecall` syscall mechanism (trap into kernel, dispatch by syscall number, return)
- Implement core syscalls: `write()`, `exit()`, `fork()`, `exec()`, `wait()`, `getpid()`, `kill()`, `sleep()`
- Virtual Memory Areas (VMAs) — per-process sorted list replacing the flat `sz` field
- ELF loader for `exec()` — parse program headers, map PT_LOAD segments, push argv
- Reference-counted pages (`page_get`/`page_put`) preparing for COW
- Timer-based sleep with precise hardware arming (sorted sleep list + min-deadline)
- Implement `sbrk()`, copy-on-write fork, and lazy allocation via page faults (Round 6-4)

> **Milestone:** user-mode init forks, execs hello, hello prints + sleeps + prints + exits. Full process tree.

### Phase 7 — File System

- Implement a virtio block device driver (QEMU `virt` machine's virtio-blk)
- Implement a buffer cache (LRU cache of disk blocks, sleep-locks)
- Implement write-ahead logging for crash recovery
- Design an on-disk format: superblock, inodes, free bitmap, data blocks
- Implement directories and path resolution (`namei`)
- Implement file syscalls: `open()`, `read()`, `write()`, `close()`, `fstat()`, `dup()`, `link()`, `unlink()` with file descriptors
- Write `mkfs` — a host-side tool to create a filesystem image

> **Milestone:** format a disk image, create/read/write files via syscalls.

### Phase 8 — Shell & Userspace

- Implement interrupt-driven UART input with line buffering
- Implement `pipe()` for inter-process communication
- Write a minimal shell: read input, parse commands, fork + exec
- Implement I/O redirection (`echo hello > file`) and pipes (`cmd1 | cmd2`)
- Implement `mmap()` (anonymous + file-backed)
- Write small userspace programs: `echo`, `cat`, `ls`, `wc`, `grep`
- Implement a minimal libc (printf, malloc/free via mmap, string functions)

> **Milestone:** boot bobchouOS, get a shell prompt, run commands. The "wow" moment.

### Phase 9 — Concurrency & Multi-core

- Wake secondary harts, implement per-CPU schedulers
- Fine-grained locking, lock ordering, deadlock prevention
- Memory barriers and hardware memory ordering
- Implement user-level threads: `clone()` syscall, futex, user-space mutex/condvar

> **Milestone:** multiple harts running independent processes; user threads with synchronization.

### Stretch Goals (Time Permitting)

Each is independent — pick any after Phase 9.

| Topic | What it teaches |
|-------|-----------------|
| **Demand paging & swap** | Page fault handler loads pages on access; evict to swap under memory pressure. |
| **Multi-queue blk & sharded buffer cache** | Per-CPU resource partitioning, fine-grained locking, concurrent completion handling. |
| **Signals** | Asynchronous notification (SIGKILL, SIGCHLD, SIGSEGV). User-space signal handlers. |
| **Networking** | virtio-net driver → minimal IP/UDP/TCP stack → socket syscall. |
| **Port a real program** | Lua interpreter or similar — proves the syscall interface is complete. |
| **Real hardware** | Boot on a physical RISC-V board (VisionFive 2). Device tree parsing. |

## Estimated Project Timeline

| Phase | Duration | Cumulative |
|-------|----------|------------|
| Phase 0 — Tooling & C | ~3 weeks | 3 weeks |
| Phase 1 — Boot & early kernel | ~4 weeks | 7 weeks |
| Phase 2 — Traps & interrupts | ~4 weeks | 11 weeks |
| Phase 3 — Physical memory | ~3 weeks | 14 weeks |
| Phase 4 — Virtual memory | ~4 weeks | 18 weeks |
| Phase 5 — Processes & scheduling | ~5 weeks | 23 weeks |
| Phase 6 — User mode & syscalls | ~4 weeks | 27 weeks |
| Phase 7 — File system | ~5 weeks | 32 weeks |
| Phase 8 — Shell & userspace | ~4 weeks | 36 weeks |
| Phase 9 — Concurrency & multi-core | ~4 weeks | 40 weeks |

Total: approximately **40 weeks (~10 months)** of active development at a part-time pace (~8 hours/week), with buffer time built into a 1-2 year horizon for breaks, difficult bugs, and deeper exploration.

## Resources & References

### Primary Reference

- **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv)** — MIT's teaching OS for RISC-V. ~6,000 lines of C. The single most important reference for this project.
- **[xv6 Book (RISC-V edition)](https://pdos.csail.mit.edu/6.828/2023/xv6/book-riscv-rev3.pdf)** — Companion textbook explaining xv6's design chapter by chapter.

### RISC-V Architecture

- **[RISC-V Privileged Specification](https://riscv.org/technical/specifications/)** — The authoritative reference for privilege modes, CSRs, traps, and paging (Sv39).
- **[RISC-V Unprivileged Specification](https://riscv.org/technical/specifications/)** — Base instruction set reference.

### OS Theory

- **[Operating Systems: Three Easy Pieces (OSTEP)](https://pages.cs.wisc.edu/~remzi/OSTEP/)** — Free online textbook. Excellent for bridging theory and practice.

### C Language

- **"The C Programming Language" (K&R)** — The classic. Short, dense, sufficient.
- **"Modern C" by Jens Gustedt** — A more contemporary take, freely available online.

### Tutorials & Community

- **[OSDev Wiki](https://wiki.osdev.org)** — Community wiki covering nearly every OS development topic.
- **[Brokenthorn OS Dev Series](http://www.brokenthorn.com/Resources/)** — Classic bare-metal OS tutorial (x86, but conceptually transferable).
- **[Writing an OS in Rust](https://os.phil-opp.com/)** — Not directly used here, but an excellent resource for a future Rust-based OS project.

## Project Structure

```
bobchouOS/
├── lectures/               # Learning materials (one per round)
│   ├── 0-1-tooling/        #   Toolchain setup + bare-metal hello
│   ├── 0-2-assembly-recap/ #   RISC-V assembly exercises
│   ├── 0-3-c-recap/        #   C fundamentals exercises
│   ├── 1-1-boot-and-linker/
│   ├── ...                 #   (one directory per round)
│
├── include/                # Shared ABI headers (kernel ↔ user)
│   ├── syscall_num.h       #   Syscall numbers (user/kernel contract)
│   └── errno.h             #   Error codes
│
├── kernel/                 # Kernel source
│   ├── arch/               #   RISC-V assembly: entry, traps, context switch, SBI
│   │                       #   + embedded user binaries (user_bin_*.S)
│   ├── include/            #   All kernel headers
│   ├── drivers/            #   UART (future: virtio-blk, PLIC)
│   ├── lib/                #   kprintf, string utilities
│   ├── test/               #   Unit tests (run via `make test`)
│   ├── main.c              #   Kernel entry point
│   ├── proc.c              #   Process management, scheduler, lifecycle
│   ├── exec.c              #   ELF loader, proc_exec
│   ├── vma.c               #   Virtual Memory Area operations
│   ├── syscall.c           #   System call dispatch + handlers
│   ├── spinlock.c          #   Spinlock (amoswap + irqsave)
│   ├── wait_queue.c        #   Wait queues (targeted sleep/wakeup)
│   ├── trap.c              #   Trap dispatcher, user/kernel trap paths
│   ├── vm.c                #   Page tables, copyin/copyout, page refcount
│   ├── kalloc.c            #   Physical page allocator (buddy)
│   └── kmalloc.c           #   Kernel heap allocator (slab)
│
├── user/                   # User-space programs
│   ├── init.c              #   PID 1: fork + exec + wait loop
│   ├── hello.c             #   Test program: write + sleep + write + exit
│   ├── start.S             #   _start: call main, then exit
│   ├── usys.S              #   Syscall stubs (li a7, ecall, ret)
│   ├── user.h              #   Syscall prototypes for user programs
│   └── user.ld             #   User linker script (text at 0x1000)
│
├── docs/                   # Reference specs (xv6 book, RISC-V privileged spec)
├── Makefile                # Build system
├── linker.ld               # Kernel linker script
├── LICENSE
└── README.md
```

Each lecture directory contains a `lecture.md` writeup and self-contained exercises. The `kernel/` directory is the actual OS — built incrementally across phases. The `user/` directory holds user-space programs compiled as ELF and embedded into the kernel (until Phase 7 adds a filesystem).

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
