# Lecture 6-2: System Call Dispatch

> **Where we are**
>
> Round 6-1 proved the user/kernel boundary works end-to-end. A tiny
> assembly program runs in U-mode, issues `ecall`, traps into the
> kernel via the trampoline, and the kernel prints the arguments before
> killing the process. The full mechanism is in place: trampoline page
> (user_vec / user_ret), per-process trapframe, page table switching,
> stvec redirection.
>
> But the kernel doesn't *do* anything with the ecall — it just prints
> and kills. There is no dispatch, no argument extraction, no return
> value. The kernel has a door, but no receptionist.
>
> This round builds the **system call layer**: the dispatch table that
> maps a number to a handler, the argument convention that lets handlers
> read user data, the return path that delivers results back to user
> code, and the first two real system calls — `write()` and `exit()`.
>
> By the end of this round, a user program will print "hello world" to
> the console and exit cleanly.
>
> **What you will understand after this lecture:**
>
> - How ecall numbers map to kernel handler functions via a dispatch table
> - How arguments pass from user registers (a0–a5) through the trapframe
> - How syscall return values flow back to user code via a0
> - How the kernel safely reads user memory (copyin)
> - How `write()` and `exit()` work as the simplest real syscalls
> - Why interrupts are enabled during syscall execution

> **xv6 book coverage:**
> This lecture absorbs Ch 4 §4.3 ("Code: Calling system calls") and
> §4.4 ("Code: System call arguments") in full. We also touch the
> beginning of Ch 1 where the user-side write/exit semantics are
> introduced.

---

## Part 1: The Big Picture

### What happens today (Round 6-1)

```
User program:
    li a7, 0        # syscall "number"
    li a0, 42       # argument
    ecall           # trap!

Kernel (user_trap):
    saves sepc to trapframe->epc
    epc += 4        # skip past ecall instruction
    prints a7 and a0
    sets killed = 1
    falls through to user_trap_ret (which sees killed → exit)
```

### What we want (Round 6-2)

```
User program:
    li a7, SYS_write   # syscall number = 1
    li a0, 1            # fd = stdout
    la a1, msg          # buffer pointer (user VA)
    li a2, 12           # length
    ecall

Kernel (user_trap):
    saves sepc to trapframe->epc
    epc += 4
    intr_on()                          ← enable interrupts
    trapframe->a0 = syscall()          ← dispatch + store return value
    user_trap_ret()

Back in user mode:
    # a0 now contains 12 (bytes written) or negative error code
```

The key additions:

1. **Dispatch function** (`syscall()`) — reads a7, indexes into a table, calls the handler
2. **Argument access** — handlers read a0–a5 directly from `p->trapframe`
3. **Return value** — handler returns int64, dispatch writes it to `trapframe->a0`
4. **copyin** — for pointer arguments, safely copy user memory into kernel buffers
5. **intr_on()** — enable interrupts so timer preemption works during syscalls

---

## Part 2: Dispatch Table & Arguments

### Syscall numbers

The syscall number in `a7` indexes into an array of function pointers:

```c
// syscall_num.h — shared between kernel and user
#define SYS_write   1    // Linux RISC-V: 64
#define SYS_exit    2    // Linux RISC-V: 93

#define NSYSCALL    3    // one past the last valid number
```

Numbers start at 1 (not 0), so number 0 is always invalid — catches uninitialized
registers. `NSYSCALL` is the array size bound.

### The dispatch table and function

```c
static int64 (*syscalls[])(void) = {
    [0]          = NULL,         // invalid
    [SYS_write]  = sys_write,
    [SYS_exit]   = sys_exit,
};

int64
syscall(void) {
    struct proc *p = this_proc();
    int num = (int)p->trapframe->a7;

    if (num > 0 && num < NSYSCALL && syscalls[num])
        return syscalls[num]();
    return -ENOSYS;    // "function not implemented"
}
```

Bounds-check, null-check, call. The caller (user_trap) writes the result to
`trapframe->a0`.

### Argument convention

RISC-V calling convention passes arguments in a0–a7. For syscalls:
- **a7** = syscall number (the "which function" selector)
- **a0–a5** = up to 6 arguments

Why a7? Using a0 would steal an argument slot. a7 is "free" — most syscalls
take fewer than 8 arguments. This matches both RISC-V Linux and xv6.

When user code does `ecall`, hardware doesn't touch the general registers — it
only modifies `sepc`, `sstatus`, and `scause`. The trampoline (user_vec) saves
all registers to the trapframe. So by the time the kernel handler runs,
`p->trapframe->a0` through `p->trapframe->a5` hold the user's arguments exactly
as passed.

Each `sys_*` handler reads from the trapframe directly:

```c
int64
sys_write(void) {
    struct proc *p = this_proc();
    int fd       = (int)p->trapframe->a0;
    uint64 uaddr = p->trapframe->a1;
    uint64 len   = p->trapframe->a2;
    // ... validate and execute ...
}
```

> **Why no argint/argaddr wrappers (yet):**
>
> xv6 has ~20 syscalls with repeated validation patterns — `argint(n, &val)`
> gives consistent error handling. For 2–3 syscalls, direct access is simpler.
> When we reach Phase 7 with 10+ syscalls, we can add helpers then.

---

## Part 3: Return Values & Error Codes

### Convention

The syscall handler returns an `int64`:
- **≥ 0**: success (byte count for write, pid for fork, etc.)
- **< 0**: error (the negation of an error code)
- **no return**: some syscalls never return (exit)

The dispatch code writes this to `trapframe->a0`. When `user_ret` restores
registers and executes `sret`, the user resumes with the result in a0.

### Error codes

We define a minimal set in `include/errno.h`:

```c
#define ENOSYS  38   // syscall not implemented
#define EFAULT  14   // bad user address
#define EBADF    9   // bad file descriptor
#define EINVAL  22   // invalid argument
```

Using Linux's actual numbers — "error 14" means EFAULT everywhere.

### User-side checking

```asm
ecall
bltz a0, error    # if negative, it's an error code
```

Or in future C user code:

```c
int n = write(1, buf, len);
if (n < 0) {
    // -n is the error code (e.g., n == -9 means EBADF)
}
```

No `errno` global needed. The kernel delivers the error directly.

---

## Part 4: Safely Reading User Memory — copyin

- **copyin**: copy bytes from a user virtual address into a kernel buffer
- **copyout**: copy bytes from a kernel buffer into a user virtual address

### The problem

When user code calls `write(1, buf, 12)`, `buf` is a virtual address in the
**user's** page table. The kernel is running with `kernel_satp`. If the kernel
just does `memcpy(kbuf, (void*)buf, 12)`, it reads from `buf` in the **kernel's**
address space — which is either unmapped (panic) or completely wrong memory.

The kernel must translate the user VA through the **user's page table** to find
the physical address, then access it through the kernel's identity map.

### The solution

```
User VA  ─── walk user PT ───→  Physical Address
                                       │
                                       ▼
Kernel VA (identity map: VA = PA) ── memcpy ──→  kernel buffer
```

Since our kernel identity-maps all physical RAM (PA == kernel VA), once we have
the PA, we can use it directly as a pointer.

### copyin implementation

```c
int
copyin(pte_t *pagetable, void *dst, uint64 srcva, uint64 len) {
    while (len > 0) {
        uint64 va_page = PG_ROUND_DOWN(srcva);
        pte_t *pte = walk(pagetable, va_page, 0);
        if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U))
            return -EFAULT;
        uint64 offset = srcva - va_page;
        uint64 pa = pte_to_pa(*pte) + offset;
        uint64 n = PG_SIZE - offset;
        if (n > len)
            n = len;
        memcpy(dst, (void *)pa, n);
        dst = (char *)dst + n;
        srcva += n;
        len -= n;
    }
    return 0;
}
```

Key points:
1. **Page-at-a-time**: buffer might span page boundaries. Must walk each page separately.
2. **Validate PTE_V and PTE_U**: reject invalid or kernel-only pages. Without PTE_U, a user could pass a VA pointing to the trapframe (mapped without PTE_U) and leak kernel data.
3. **No allocation**: `walk(pagetable, va, 0)` — read-only traversal.
4. **Identity map assumption**: `(void *)pa` works because kernel maps all DRAM at PA == VA.
5. **memcpy, not memmove**: src (user physical page) and dst (kernel buffer) never overlap.
6. **No PTE_R check**: RISC-V allows execute-only pages (X=1, R=0). We don't check — same as xv6. We never create such pages, so this is safe.

### copyout (kernel → user)

Same pattern reversed, with an additional `PTE_W` check:

```c
int
copyout(pte_t *pagetable, uint64 dstva, void *src, uint64 len) {
    while (len > 0) {
        uint64 va_page = PG_ROUND_DOWN(dstva);
        pte_t *pte = walk(pagetable, va_page, 0);
        if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U) || !(*pte & PTE_W))
            return -EFAULT;
        uint64 offset = dstva - va_page;
        uint64 pa = pte_to_pa(*pte) + offset;
        uint64 n = PG_SIZE - offset;
        if (n > len)
            n = len;
        memcpy((void *)pa, src, n);
        dstva += n;
        src = (char *)src + n;
        len -= n;
    }
    return 0;
}
```

xv6's `copyinstr` (kernel/vm.c:403) is nearly identical. xv6 also has
`copyinstr` for null-terminated strings — we'll add that for exec in Round 6-3.

---

## Part 5: The First Syscalls

### Enabling interrupts

When `user_vec` traps into the kernel, hardware copies SIE to SPIE, then clears
`sstatus.SIE`. We re-enable interrupts before dispatching because:
- Timer must fire for preemption (a long syscall would starve other processes)
- Future blocking syscalls (wait, read) need the timer for wakeup

By the time we call `intr_on()`, all preconditions hold: `stvec` points to
`kernel_vec`, we're on the kernel stack with `kernel_satp`, and `sepc` is saved.

### Wiring into user_trap

The Round 6-1 placeholder:

```c
case EXC_ECALL_U:
    p->trapframe->epc += 4;
    kprintf("...");
    p->killed = 1;
    break;
```

Becomes:

```c
case EXC_ECALL_U:
    p->trapframe->epc += 4;
    intr_on();
    p->trapframe->a0 = syscall();
    break;
```

Three lines: advance past ecall, enable interrupts, dispatch and store result.

### sys_write

```c
static int64
sys_write(void) {
    struct proc *p = this_proc();
    int fd       = (int)p->trapframe->a0;
    uint64 uaddr = p->trapframe->a1;
    uint64 len   = p->trapframe->a2;

    if (fd != 1)
        return -EBADF;
    if (len > 1024)
        return -EINVAL;

    char kbuf[128];
    uint64 written = 0;
    while (written < len) {
        uint64 chunk = len - written;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);
        if (copyin(p->pagetable, kbuf, uaddr + written, chunk) < 0)
            return -EFAULT;
        for (uint64 i = 0; i < chunk; i++)
            uart_putc(kbuf[i]);
        written += chunk;
    }
    return (int64)written;
}
```

Design decisions:
- **fd as int**: small bounded index, cast for POSIX readability. `uaddr`/`len` stay uint64 — they feed directly into copyin.
- **fd = 1 only**: no fd table yet. Phase 7 replaces this check with `fd_lookup()`.
- **Max 1024**: caps kernel time per syscall. No negative check needed — uint64 makes "negative" user values wrap to huge positives, caught by the same bound.
- **Chunked copyin**: 128-byte stack buffer avoids heap allocation. Loops for longer writes.
- **Polling uart_putc**: simple and immediate. Phase 8-1 adds buffered TX.

### sys_exit

```c
static int64
sys_exit(void) {
    proc_exit((int)this_proc()->trapframe->a0);
    return 0;  // unreachable
}
```

A thin wrapper: extract status, call `proc_exit()`.

### The rename: exit → proc_exit, wait → proc_wait, kill → proc_kill

The `sys_*` handlers are syscall entry points. The kernel functions they call are
internal implementations — the bare names (`exit`, `wait`, `kill`) belong to
future userland libc.

| Old name | New name | Why |
|----------|----------|-----|
| `exit(int status)` | `proc_exit(int status)` | `exit` is the user-side name |
| `wait(int *status)` | `proc_wait(int *status)` | `wait` is the user-side name |
| `kill(int pid)` | `proc_kill(int pid)` | `kill` is the user-side name |

The `proc_*` prefix is consistent with existing `proc_init()`, `proc_create_kernel()`.

---

## Part 6: The User Program — "Hello World"

### Assembly (replaces test_user.S)

```asm
#include "syscall_num.h"

.section .text
.globl _start

_start:
    # write(1, msg, 12)
    li  a7, SYS_write
    li  a0, 1           # fd = stdout
    la  a1, msg         # buffer address
    li  a2, 12          # length
    ecall

    # exit(0)
    li  a7, SYS_exit
    li  a0, 0           # status = 0
    ecall

    # unreachable
spin:
    j   spin

.section .rodata
msg:
    .ascii "hello world\n"
```

### What happens at runtime

```
 1. scheduler picks user process, calls user_trap_ret
 2. user_ret switches to user PT, restores regs, sret → _start
 3. User: loads a7=1, a0=1, a1=&msg, a2=12, ecall
 4. Hardware: sepc←PC, scause←8, SIE←0, SPP←0, PC←stvec(trampoline)
 5. user_vec: save regs → trapframe, load kernel_satp/sp, jump user_trap
 6. user_trap: epc+=4, intr_on(), calls syscall()
 7. syscall(): a7=1 → sys_write()
 8. sys_write(): copyin → walks user PT → gets PA → uart_putc × 12 → returns 12
 9. user_trap: trapframe->a0 = 12 → user_trap_ret → user_ret → sret
10. User resumes after first ecall. Loads a7=2, a0=0, ecall
11. Same trap path → sys_exit() → proc_exit(0) → zombie → init reaps
```

Console output: `hello world\n`

---

## Part 7: Security — The Trust Boundary

The syscall layer is the **trust boundary** between user and kernel. Every piece
of data from the user is potentially malicious. The handler must validate
everything:

1. **Bounds-check the syscall number** — out-of-range a7 must not index past the table
2. **Validate file descriptors** — must refer to a real open file (for now, only fd=1)
3. **Validate lengths** — absurdly large values rejected (uint64 wraps "negative" to huge positive, caught by the same bound)
4. **Validate user pointers via copyin** — never dereference directly; always walk the page table

### What copyin prevents

Without copyin, a malicious user could:
- Pass a kernel VA → kernel reads its own secrets and sends them to console
- Pass the trapframe VA (mapped without PTE_U) → same issue
- Pass an unmapped VA → kernel page fault → panic (DoS)

With copyin:
- Only pages with PTE_U are accessible
- Invalid/unmapped VA → returns -EFAULT → user gets an error, kernel continues

The trapframe is the critical case: mapped in the user page table at VA
`TRAPFRAME` but **without PTE_U**. Hardware blocks user access in U-mode;
copyin blocks kernel-side reads on behalf of the user. Both checks are needed.

### What's next

Round 6-3: fork copies user address space, exec() with ELF loader,
wait/kill/getpid as syscalls, first real user init that forks and execs.

---

## Quick Reference

### File layout

```
include/
    syscall_num.h       # SYS_write=1, SYS_exit=2, NSYSCALL (ABI contract)
    errno.h             # ENOSYS, EFAULT, EBADF, EINVAL

kernel/
    syscall.c           # dispatch table + sys_write + sys_exit
    vm.c                # copyin(), copyout() added
    trap.c              # ecall handler: intr_on + syscall()
    proc.c              # exit→proc_exit, wait→proc_wait, kill→proc_kill
```

### bobchouOS vs xv6

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Syscall number register | a7 | a7 |
| Dispatch table | fn pointer array | same |
| Error return | -1 (no errno) | -ERRNO (negative error code) |
| Argument access | argint/argaddr helpers | direct trapframe access |
| User memory copy | copyin/copyout/copyinstr | copyin/copyout (copyinstr in 6-3) |
| Interrupts in syscall | intr_on() in usertrap | intr_on() in user_trap |
| write() fd support | full fd table | fd==1 hardcoded (Phase 7) |
| Naming | exit() directly | sys_exit() → proc_exit() |
| File organization | syscall.c + sysproc.c + sysfile.c | single syscall.c |

### Syscall numbers

| Number | Name | Linux RISC-V | Args | Returns |
|--------|------|-------------|------|---------|
| 1 | write | 64 | fd, buf, len | bytes written or -error |
| 2 | exit | 93 | status | never returns |

### Error codes

| Code | Name | Meaning |
|------|------|---------|
| 9 | EBADF | Bad file descriptor |
| 14 | EFAULT | Bad user address |
| 22 | EINVAL | Invalid argument |
| 38 | ENOSYS | Syscall not implemented |

### Register convention

| Register | Role |
|----------|------|
| a7 | Syscall number |
| a0 | Arg 1 / return value |
| a1 | Arg 2 |
| a2 | Arg 3 |
| a3 | Arg 4 |
| a4 | Arg 5 |
| a5 | Arg 6 |

### Control flow

```
User:    ecall
           │
Hardware:  sepc←PC, scause←8, SIE←0, SPP←0, PC←stvec(trampoline)
           │
user_vec:  save regs → trapframe, load kernel_satp/sp
           │
user_trap: epc+=4, intr_on(), ret = syscall()
           │                           │
           │                   reads a7 → table[a7]()
           │                           │
           │                   sys_write / sys_exit / ...
           │                           │
           │                   returns int64
           │
           trapframe->a0 = ret
           │
user_trap_ret: set stvec, sepc, sstatus → user_ret
           │
user_ret:  restore regs ← trapframe, switch to user PT, sret
           │
User:      resumes at ecall+4, a0 = return value
```
