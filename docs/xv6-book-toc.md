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

The xv6 book is organized by *concept*, while bobchouOS phases follow *build order*. Some chapters split across multiple phases.

| xv6 Book Chapter | bobchouOS Phase | What to read and when |
|---|---|---|
| **Ch 1** — OS interfaces | **Phase 6 & 8** | Read as a preview early on; the syscall interfaces (`fork`, `exec`, `pipe`, `open`) become real when you implement them |
| **Ch 2** — OS organization | **Phase 1 & 2** | 2.1–2.5 (kernel organization, privilege modes) for Phase 1; 2.6 (starting xv6, first process) spans Phases 1–5 |
| **Ch 3** — Page tables | **Phase 3, 4 & 6** | 3.4–3.5 (physical allocator) = Phase 3; 3.1–3.3 (Sv39, kernel address space) = Phase 4; 3.6–3.8 (process address space, sbrk, exec) = Phase 6 |
| **Ch 4** — Traps and syscalls | **Phase 2 & 6** | 4.1, 4.5 (trap machinery, kernel traps) = Phase 2; 4.2–4.4 (user traps, syscall dispatch) = Phase 6; 4.6 (page faults) = Phase 4 |
| **Ch 5** — Interrupts and drivers | **Phase 1 & 2** | 5.1–5.2 (console/UART output) = Phase 1; 5.3–5.4 (concurrency, timer interrupts) = Phase 2 |
| **Ch 6** — Locking | **Phase 5** | Needed once you have multiple processes and concurrency |
| **Ch 7** — Scheduling | **Phase 5 & 8** | 7.1–7.4 (context switching, scheduler) = Phase 5; 7.5–7.8 (sleep/wakeup, pipes, wait/exit) = Phases 5 & 8 |
| **Ch 8** — File system | **Phase 7** | Maps almost 1:1 (buffer cache, logging, inodes, directories, file descriptors) |
| **Ch 9** — Concurrency revisited | **Phase 5+** | Advanced locking patterns; revisit after basic scheduling works |
| **Ch 10** — Summary | — | Wrap-up |
