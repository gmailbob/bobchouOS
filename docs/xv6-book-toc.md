# xv6 Book (RISC-V edition) — Table of Contents

Source: *xv6: a simple, Unix-like teaching operating system* (RISC-V edition, rev3), Russ Cox, Frans Kaashoek, Robert Morris, 2022.

Note: the book has no chapter numbers — these are labeled Ch 1–10 by convention.

## Ch 1 — Operating system interfaces (p.9)
- Processes and memory (p.10)
- I/O and File descriptors (p.13)
- Pipes (p.16)
- File system (p.17)
- Real world (p.19)
- Exercises (p.19)

## Ch 2 — Operating system organization (p.21)
- Abstracting physical resources (p.22)
- User mode, supervisor mode, and system calls (p.22)
- Kernel organization (p.23)
- Code: xv6 organization (p.25)
- Process overview (p.26)
- Code: starting xv6, the first process and system call (p.27)
- Security Model (p.28)
- Real world (p.29)
- Exercises (p.29)

## Ch 3 — Page tables (p.31)
- Paging hardware (p.31)
- Kernel address space (p.34)
- Code: creating an address space (p.35)
- Physical memory allocation (p.37)
- Code: Physical memory allocator (p.37)
- Process address space (p.38)
- Code: sbrk (p.39)
- Code: exec (p.40)
- Real world (p.41)
- Exercises (p.42)

## Ch 4 — Traps and system calls (p.43)
- RISC-V trap machinery (p.44)
- Traps from user space (p.45)
- Code: Calling system calls (p.47)
- Code: System call arguments (p.47)
- Traps from kernel space (p.48)
- Page-fault exceptions (p.48)
- Real world (p.51)
- Exercises (p.51)

## Ch 5 — Interrupts and device drivers (p.53)
- Code: Console input (p.53)
- Code: Console output (p.54)
- Concurrency in drivers (p.55)
- Timer interrupts (p.55)
- Real world (p.56)
- Exercises (p.57)

## Ch 6 — Locking (p.59)
- Races (p.60)
- Code: Locks (p.63)
- Code: Using locks (p.64)
- Deadlock and lock ordering (p.64)
- Re-entrant locks (p.66)
- Locks and interrupt handlers (p.67)
- Instruction and memory ordering (p.67)
- Sleep locks (p.68)
- Real world (p.69)
- Exercises (p.69)

## Ch 7 — Scheduling (p.71)
- Multiplexing (p.71)
- Code: Context switching (p.72)
- Code: Scheduling (p.73)
- Code: mycpu and myproc (p.74)
- Sleep and wakeup (p.75)
- Code: Sleep and wakeup (p.78)
- Code: Pipes (p.79)
- Code: Wait, exit, and kill (p.80)
- Process Locking (p.81)
- Real world (p.82)
- Exercises (p.84)

## Ch 8 — File system (p.85)
- Overview (p.85)
- Buffer cache layer (p.87)
- Code: Buffer cache (p.87)
- Logging layer (p.88)
- Log design (p.89)
- Code: logging (p.90)
- Code: Block allocator (p.91)
- Inode layer (p.92)
- Code: Inodes (p.93)
- Code: Inode content (p.94)
- Code: directory layer (p.96)
- Code: Path names (p.96)
- File descriptor layer (p.97)
- Code: System calls (p.98)
- Real world (p.99)
- Exercises (p.100)

## Ch 9 — Concurrency revisited (p.103)
- Locking patterns (p.103)
- Lock-like patterns (p.104)
- No locks at all (p.105)
- Parallelism (p.105)
- Exercises (p.106)

## Ch 10 — Summary (p.107)

---

## Mapping to bobchouOS Phases

The xv6 book is organized by *concept*, while bobchouOS phases follow *build order*. Some chapters split across multiple phases; some phases draw from multiple chapters.

### Ch 1 — OS interfaces → Phase 6, 7, 8

A preview/overview chapter. The syscall API it describes (`fork`, `exec`, `pipe`, `open`, `read`, `write`) becomes real as you implement each subsystem. Worth reading early for context, then re-reading as you build each piece.

### Ch 2 — OS organization → Phase 1, 2, 5

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| Privilege modes (2.2) | Phase 2 | M/S/U mode, ecall, trap mechanism |
| Kernel organization (2.3–2.5) | Phase 1 | Monolithic kernel, code layout |
| Process overview (2.6) | Phase 5 | struct proc, address space, state machine |
| Starting xv6, first process (2.7) | Phase 5-1, 6-3 | Kernel bootstrap = 5-1; user init = 6-3 |

### Ch 3 — Page tables → Phase 3, 4, 6

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| Paging hardware, Sv39 (3.1) | Phase 4-1 | Three-level page table walk |
| Kernel address space (3.2–3.3) | Phase 4-1 | Identity map, device MMIO mappings |
| Physical memory allocator (3.4–3.5) | Phase 3-2 | kalloc/kfree free list |
| Process address space (3.6) | Phase 6-1 | Per-process user page table |
| Code: sbrk (3.7) | Phase 6-4 | Grow/shrink user heap |
| Code: exec (3.8) | Phase 6-3 | ELF loading into user address space |

### Ch 4 — Traps and system calls → Phase 2, 5, 6

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| RISC-V trap machinery (4.1) | Phase 2-1 | stvec, sepc, scause, sstatus, sscratch |
| Traps from user space (4.2) | Phase 6-1 | Trampoline, uservec/userret, trapframe |
| Calling system calls (4.3) | Phase 6-2 | ecall dispatch, syscall table |
| System call arguments (4.4) | Phase 6-2 | argint, argaddr, argfd |
| Traps from kernel space (4.5) | Phase 2-2, 5-1 | kernel_vec, timer interrupt, ret_from_trap |
| Page-fault exceptions (4.6) | Phase 6-4 | COW fork, lazy allocation, demand paging |

### Ch 5 — Interrupts and device drivers → Phase 1, 2, 7, 8

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| Console output (5.2) | Phase 1-2 | UART polling driver |
| Console input (5.1) | Phase 8-1 | Interrupt-driven RX buffer |
| Concurrency in drivers (5.3) | Phase 8-1 | Lock + sleep/wakeup in console driver |
| Timer interrupts (5.4) | Phase 2-3, 5-1 | CLINT timer, mini-SBI |
| *virtio-blk (implied)* | Phase 7-1 | Block device driver |

### Ch 6 — Locking → Phase 5, 7, 9

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| Races + Code: Locks (6.1–6.2) | Phase 5-2 | Spinlock implementation (amoswap) |
| Using locks (6.3) | Phase 5-2 | Protecting process state, allocator |
| Deadlock and lock ordering (6.4) | Phase 9-2 | Global lock order, multi-lock scenarios |
| Locks and interrupt handlers (6.6) | Phase 5-2 | push_off/pop_off, disable interrupts while holding |
| Instruction and memory ordering (6.7) | Phase 9-2 | fence, __sync_synchronize, compiler barriers |
| Sleep locks (6.8) | Phase 7-2 | Buffer cache needs locks held across I/O |

### Ch 7 — Scheduling → Phase 5, 8

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| Context switching (7.1–7.2) | Phase 5-1 | swtch.S, struct context |
| Scheduling (7.3) | Phase 5-1 | Round-robin, scheduler loop, yield |
| mycpu and myproc (7.4) | Phase 5-1 | tp register, per-CPU state |
| Sleep and wakeup (7.5–7.6) | Phase 5-2 | Lost wakeup, condition-lock pattern |
| Pipes (7.7) | Phase 8-2 | Sleep/wakeup in practice |
| Wait, exit, and kill (7.8) | Phase 5-2 | ZOMBIE, reparenting, deferred kill |
| Process Locking (7.9) | Phase 5-2 | p->lock invariants across state transitions |

### Ch 8 — File system → Phase 7

Nearly 1:1 mapping:

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| Buffer cache (8.2–8.3) | Phase 7-2 | bread/bwrite/brelse, LRU |
| Logging (8.4–8.6) | Phase 7-3 | Write-ahead log, crash recovery |
| Block allocator (8.7) | Phase 7-4 | Free bitmap |
| Inodes (8.8–8.10) | Phase 7-4 | On-disk + in-memory inode, direct/indirect |
| Directory layer (8.11) | Phase 7-5 | dirlookup, dirlink |
| Path names (8.12) | Phase 7-5 | namei, nameiparent |
| File descriptor layer (8.13) | Phase 7-6 | struct file, fd table |
| System calls (8.14) | Phase 7-6 | open/read/write/close/link/unlink |

### Ch 9 — Concurrency revisited → Phase 9

| Section | bobchouOS | Notes |
|---------|-----------|-------|
| Locking patterns (9.1) | Phase 9-2 | Fine-grained locks, per-object locks |
| Lock-like patterns (9.2) | Phase 9-2 | Reference counting, ownership |
| No locks at all (9.3) | Phase 9-2 | Atomic instructions, started variable |
| Parallelism (9.4) | Phase 9-1, 9-2 | Multi-hart workloads, contention measurement |

### Ch 10 — Summary

Revisit after completing Phase 9. Reflects on the full system.

---

## Topics beyond xv6 (covered in bobchouOS stretch goals)

xv6 explicitly notes these as absent but present in real systems:
- Signals and process groups
- Memory-mapped files (mmap)
- Demand paging and swap
- Networking (sockets, TCP/IP)
- User-level threads (pthreads)
- Priority scheduling, real-time guarantees
- DMA, scatter/gather I/O
- Snapshots, journaling alternatives to logging
