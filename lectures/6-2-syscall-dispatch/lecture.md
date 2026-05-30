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

## Part 1: The Big Picture — From ecall to Handler and Back

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
    intr_on()                          ← NEW: enable interrupts
    ret = syscall()                    ← NEW: dispatch to sys_write()
    trapframe->a0 = ret                ← NEW: return value to user
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

## Part 2: The Dispatch Table

### Design

The syscall number in `a7` indexes into an array of function pointers:

```c
// syscall_num.h — shared between kernel and user
#define SYS_write   1    // Linux RISC-V: 64
#define SYS_exit    2    // Linux RISC-V: 93

#define NSYSCALL    3    // one past the last valid number
```

Numbers start at 1 (not 0), so number 0 is always invalid — catches uninitialized
registers. `NSYSCALL` is the array size bound.

```c
// syscall.c — dispatch table
static int64 (*syscalls[])(void) = {
    [0]          = NULL,         // invalid
    [SYS_write]  = sys_write,
    [SYS_exit]   = sys_exit,
};
```

### The dispatch function

```c
int64
syscall(void) {
    struct proc *p = this_proc();
    int num = (int)p->trapframe->a7;

    if (num > 0 && num < NSYSCALL && syscalls[num])
        return syscalls[num]();
    return -ENOSYS;    // "function not implemented"
}
```

Simple: bounds-check, null-check, call. Returns the handler's result or `-ENOSYS`
if the number is invalid. The caller (user_trap) writes the result to `trapframe->a0`.

### xv6 comparison

xv6 does the same thing (kernel/syscall.c:132):

```c
void syscall(void) {
    struct proc *p = myproc();
    int num = p->trapframe->a7;
    if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
        p->trapframe->a0 = syscalls[num]();
    } else {
        p->trapframe->a0 = -1;
    }
}
```

Differences from bobchouOS:
- xv6 writes to `trapframe->a0` inside `syscall()` — we return the value and let the caller write it (slightly cleaner separation)
- xv6 returns `-1` for unknown syscalls (no error codes) — we return `-ENOSYS`

---

## Part 3: Argument Passing

### The convention

RISC-V calling convention passes the first 8 arguments in registers a0–a7.
For system calls:
- **a7** = syscall number (not an argument — it's the "which function" selector)
- **a0–a5** = up to 6 arguments

When user code does `ecall`, hardware doesn't touch the general registers — it
only modifies `sepc` (to point at the ecall instruction) and `sstatus`/`scause`.
The trampoline (user_vec) saves all registers to the trapframe. So by the time
the kernel handler runs, `p->trapframe->a0` through `p->trapframe->a5` hold
the user's arguments exactly as they were at the ecall.

### Reading arguments in handlers

Each `sys_*` handler simply reads from the trapframe:

```c
int64
sys_write(void) {
    struct proc *p = this_proc();
    int fd       = (int)p->trapframe->a0;
    uint64 uaddr = p->trapframe->a1;
    int len      = (int)p->trapframe->a2;
    // ... validate and execute ...
}
```

No wrapper functions needed (unlike xv6's `argint`/`argaddr`/`argfd`). Direct
access is clearer when you only have a few syscalls. We can add helper functions
later if patterns emerge, but for now the code reads naturally.

> **Why xv6 uses argint/argaddr/argfd:**
>
> xv6 has ~20 syscalls with repeated validation patterns. The `argint(n, &val)`
> style lets it print a consistent error message and return -1 on any argument
> fetch failure. For 2–3 syscalls, direct access is simpler. When we reach Phase 7
> with 10+ syscalls, we can add helpers then — no refactor needed, just convenience
> wrappers.

### Why a7 for the number (not a0)?

Using a0 for the number would steal an argument slot. Using a7 (the last argument
register) is "free" — most syscalls take fewer than 8 arguments, so a7 is unused
by the actual parameters anyway.

This matches both the RISC-V Linux convention and xv6.

---

## Part 4: Return Values

### Convention

The syscall handler returns an `int64`:
- **≥ 0**: success (the meaning depends on the syscall — byte count for write, pid for fork)
- **< 0**: error (the negation of an error code)
- **no return**: some syscalls never return (exit)

The dispatch code writes this value into `trapframe->a0`. When `user_ret` restores
registers and executes `sret`, the user process resumes with the result in a0 —
exactly where the C calling convention expects a return value.

```c
// In user_trap, after syscall() returns:
p->trapframe->a0 = syscall();
```

### Error codes

We define a minimal set in a new header:

```c
// include/errno.h
#define ENOSYS  38   // syscall not implemented
#define EFAULT  14   // bad user address
#define EBADF    9   // bad file descriptor
#define EINVAL  22   // invalid argument
```

Using Linux's actual numbers (not sequential) so the values match if you ever
look up "what's error 14?" — it's always EFAULT everywhere.

### User-side checking

```asm
ecall
# a0 now holds the result
bltz a0, error    # if negative, it's an error code
# success path...
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

## Part 5: Safely Reading User Memory — copyin

- **copyin**: copy bytes from a user virtual address into a kernel buffer
- **copyout**: copy bytes from a kernel buffer into a user virtual address

### The problem

When user code calls `write(1, buf, 12)`, `buf` is a virtual address in the
**user's** page table. The kernel is running with its own page table
(`kernel_satp`). If the kernel just does `memcpy(kbuf, (void*)buf, 12)`,
it will read from address `buf` in the **kernel's** address space — which is
either unmapped (page fault → panic) or maps to completely wrong physical memory.

The kernel must translate the user VA through the **user's page table** to find
the physical address, then access that physical memory through the kernel's
identity map.

### The solution: software page table walk

```
User VA  ─── walk user PT ───→  Physical Address
                                       │
                                       ▼
Kernel VA (identity map: VA = PA) ── memcpy ──→  kernel buffer
```

Since our kernel identity-maps all physical RAM (PA == kernel VA for any address
in the DRAM range), once we have the PA, we can just use it directly as a
pointer.

### copyin implementation

```c
int
copyin(pte_t *pagetable, void *dst, uint64 srcva, uint64 len) {
    while (len > 0) {
        uint64 va_page = PGROUNDDOWN(srcva);
        pte_t *pte = walk(pagetable, va_page, 0);  // don't alloc
        if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U))
            return -EFAULT;
        uint64 pa = pte_to_pa(*pte) + (srcva - va_page);
        uint64 n = PG_SIZE - (srcva - va_page);  // bytes left in this page
        if (n > len)
            n = len;
        memcpy(dst, (void *)pa, n);
        len -= n;
        srcva += n;
        dst = (char *)dst + n;
    }
    return 0;
}
```

Key points:
1. **Page-at-a-time**: user buffer might span page boundaries (e.g., address 0x1FF0 + length 32 crosses into the next page). Must walk each page separately.
2. **Validate PTE_V and PTE_U**: reject invalid PTEs and kernel-only pages. Without the PTE_U check, a malicious user could pass a VA pointing to the trapframe (mapped without PTE_U) and trick the kernel into leaking kernel data.
3. **No allocation**: `walk(pagetable, va, 0)` — we only read, never create PTEs.
4. **Uses pte_to_pa()**: our existing helper extracts the physical address from a PTE.
5. **Identity map assumption**: `(void *)pa` works because kernel maps all DRAM at PA == VA.
6. **memcpy, not memmove**: src (user physical page) and dst (kernel buffer) are in completely different memory regions — never overlapping. Plain forward copy is safe.
7. **No PTE_R check**: the RISC-V spec allows execute-only pages (X=1, R=0). We don't check PTE_R here — same choice as xv6. In practice, we never create such pages in bobchouOS, so this is safe.

### copyout (kernel → user)

Same pattern in reverse — for future syscalls like `read()` that write data into
user buffers:

```c
int
copyout(pte_t *pagetable, uint64 dstva, void *src, uint64 len) {
    while (len > 0) {
        uint64 va_page = PGROUNDDOWN(dstva);
        pte_t *pte = walk(pagetable, va_page, 0);
        if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U) || !(*pte & PTE_W))
            return -EFAULT;
        uint64 pa = pte_to_pa(*pte) + (dstva - va_page);
        uint64 n = PG_SIZE - (dstva - va_page);
        if (n > len)
            n = len;
        memcpy((void *)pa, src, n);
        len -= n;
        dstva += n;
        src = (char *)src + n;
    }
    return 0;
}
```

Note the extra `PTE_W` check — we must not write to a read-only user page.

### xv6 comparison

xv6's `copyinstr` (kernel/vm.c:403) is nearly identical. The main difference is
xv6 also has `copyinstr` for null-terminated strings (stops at '\0'), which we'll
add when we need it (exec in Round 6-3).

| Feature | xv6 | bobchouOS |
|---------|-----|-----------|
| Walks user PT in software | yes | yes |
| Uses walkaddr() wrapper | yes (returns PA directly) | no — we use walk() + pte_to_pa() inline |
| Checks PTE_U | yes | yes |
| Page-at-a-time loop | yes | yes |
| Identity map for final access | yes (kernel maps all DRAM) | yes (same) |

---

## Part 6: Enabling Interrupts During Syscalls

### Why

When `user_vec` traps into the kernel, RISC-V hardware copies SIE to SPIE, then
clears `sstatus.SIE` — interrupts are disabled. This is necessary during the critical trampoline code
(switching page tables, loading kernel sp). But once we're in `user_trap` with
the kernel fully set up, there's no reason to keep interrupts off.

If interrupts stay disabled during a syscall:
- The timer never fires → no preemption → a long syscall starves other processes
- Future blocking syscalls (wait, read) need to sleep, which needs the timer for wakeup

xv6 enables interrupts in `usertrap()` right after setting `stvec` to `kernelvec`:

```c
// xv6 usertrap (simplified)
w_stvec((uint64)kernelvec);
...
intr_on();            // <-- here
syscall();
```

We do the same — one line in `user_trap`, before calling `syscall()`.

### Safety

By the time we call `intr_on()`, all the preconditions hold:
- `stvec` points to `kernel_vec` (set at the top of user_trap)
- We're on the kernel stack
- We're using `kernel_satp`
- `sepc` is saved to `p->trapframe->epc`

If a timer fires during syscall execution, `kernel_vec` → `kernel_trap` →
`kernel_trap_ret` handles it correctly (it saves/restores sepc and sstatus,
which is why we added that in Round 6-1).

---

## Part 7: The First Syscalls

### sys_write

```c
int64
sys_write(void) {
    struct proc *p = this_proc();
    int fd       = (int)p->trapframe->a0;
    uint64 uaddr = p->trapframe->a1;
    int len      = (int)p->trapframe->a2;

    if (fd != 1)
        return -EBADF;
    if (len < 0 || len > 1024)
        return -EINVAL;

    char kbuf[128];
    int written = 0;
    while (written < len) {
        int chunk = len - written;
        if (chunk > (int)sizeof(kbuf))
            chunk = (int)sizeof(kbuf);
        if (copyin(p->pagetable, kbuf, uaddr + written, chunk) < 0)
            return -EFAULT;
        for (int i = 0; i < chunk; i++)
            uart_putc(kbuf[i]);
        written += chunk;
    }
    return written;
}
```

Key design decisions:
- **fd = 1 only**: we don't have a file descriptor table yet. Only stdout exists.
- **Max length 1024**: prevents a user from making the kernel spin for too long in one syscall (arbitrary but reasonable for now).
- **Chunked copyin**: a 128-byte kernel stack buffer avoids allocating heap memory. Loops for longer writes.
- **Returns bytes written**: the standard write() contract. For console output, always succeeds fully (partial writes happen with files/pipes later).
- **Polling uart_putc**: simple and immediate. Phase 8-1 adds interrupt-driven buffered TX.

### sys_exit

```c
int64
sys_exit(void) {
    struct proc *p = this_proc();
    int status = (int)p->trapframe->a0;
    proc_exit(status);
    // never returns
    return 0;  // unreachable, but silences compiler warning
}
```

This is a thin wrapper: extract the status argument, call the existing kernel
`proc_exit()` (previously named `exit()` — renamed this round to avoid collision
with the user-facing name).

### The rename: exit → proc_exit, wait → proc_wait, kill → proc_kill

These kernel functions are internal implementations — they're not syscalls
themselves. The `sys_*` handlers are the syscall entry points that extract
arguments and call these internals. The names `exit`, `wait`, `kill` belong
to userland (they're what libc will provide).

| Old name | New name | Why |
|----------|----------|-----|
| `exit(int status)` | `proc_exit(int status)` | `exit` is the user-side function name |
| `wait(int *status)` | `proc_wait(int *status)` | `wait` is the user-side function name |
| `kill(int pid)` | `proc_kill(int pid)` | `kill` is the user-side function name |

The `proc_*` prefix is consistent with existing `proc_init()`, `proc_create_kernel()`.

---

## Part 8: Integration — Wiring It Into user_trap

### Current user_trap ecall handling (Round 6-1)

```c
case EXC_ECALL_U:
    p->trapframe->epc += 4;
    kprintf("user_trap: ecall from user mode, pid=%d, a7=%d, a0=%d\n", ...);
    p->killed = 1;
    break;
```

### New ecall handling (Round 6-2)

```c
case EXC_ECALL_U:
    p->trapframe->epc += 4;
    intr_on();
    p->trapframe->a0 = syscall();
    break;
```

That's it. Three lines replace the print-and-kill placeholder:
1. Advance past ecall (unchanged)
2. Enable interrupts
3. Dispatch and store result

The process continues running after the syscall (unless it called exit, in which
case `proc_exit()` never returns — `user_trap_ret` is never reached for that process).

---

## Part 9: The User Program — "Hello World"

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
3. User executes: loads a7=1, a0=1, a1=&msg, a2=12, ecall
4. Hardware: sepc←PC, scause←8 (ecall-from-U), SIE←0, SPP←0, PC←stvec
5. user_vec: save all regs to trapframe, load kernel_satp/sp, jump user_trap
6. user_trap: epc += 4, intr_on(), calls syscall()
7. syscall(): reads a7=1, dispatches to sys_write()
8. sys_write(): reads fd=1, uaddr=&msg, len=12
             copyin(pagetable, kbuf, &msg, 12) → walks user PT → gets PA
             uart_putc('h'), uart_putc('e'), ..., uart_putc('\n')
             returns 12
9. user_trap: trapframe->a0 = 12, falls through to user_trap_ret
10. user_trap_ret → user_ret → sret back to user (after the first ecall)
11. User executes: loads a7=2, a0=0, ecall
12. Same trap path → syscall() dispatches to sys_exit()
13. sys_exit(): calls proc_exit(0) → reparent, zombie, sched() — never returns
14. init reaps the zombie, prints nothing extra
```

Console output: `hello world\n`

---

## Part 10: File Layout

### New files

```
include/
    syscall_num.h       # SYS_write=1, SYS_exit=2, NSYSCALL
    errno.h             # ENOSYS, EFAULT, EBADF, EINVAL

kernel/
    syscall.c           # dispatch table + sys_write + sys_exit
```

### Modified files

```
kernel/trap.c           # user_trap ecall: replace print+kill with intr_on + syscall()
                        # kernel_trap_ret & user_trap_ret: exit(-1) → proc_exit(-1)
kernel/vm.c             # add copyin(), copyout()
kernel/include/vm.h     # declare copyin(), copyout()
kernel/proc.c           # rename exit→proc_exit, wait→proc_wait, kill→proc_kill
                        # (includes internal call in proc_wait: exit(-1) → proc_exit(-1))
kernel/include/proc.h   # rename declarations
kernel/test/test_proc.c # update calls to renamed functions (if any)
user/test_user.S        # replace with hello-world program
```

### Why a top-level `include/` directory?

`syscall_num.h` and `errno.h` define the **user/kernel ABI contract**. They don't
belong inside `kernel/include/` (that's kernel-internal) and don't belong in
`user/` (that's going away). A neutral `include/` says "this is the interface
between the two worlds."

The kernel's Makefile adds `-I include/` so kernel code can `#include "syscall_num.h"`.

---

## Part 11: bobchouOS vs xv6 — Comparison Table

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Syscall number register | a7 | a7 |
| Dispatch table | array of fn pointers, indexed by number | same |
| Error return | -1 (no errno) | -ERRNO (negative error code) |
| Argument access | argint(n, &val) / argaddr(n, &val) helpers | direct trapframe access |
| User mem copy | copyin/copyout/copyinstr | copyin/copyout (copyinstr in 6-3) |
| PT walk for copyin | walkaddr() wrapper | walk() + pte_to_pa() |
| Interrupts in syscall | intr_on() in usertrap | intr_on() in user_trap |
| write() fd support | full fd table | fd==1 hardcoded (Phase 7 adds table) |
| exit() | exit(status) directly | sys_exit() calls proc_exit() |
| Syscall file | kernel/syscall.c + kernel/sysproc.c + kernel/sysfile.c | single kernel/syscall.c |

---

## Part 12: Security Considerations

### Trust boundary

The syscall layer is the **trust boundary** between user and kernel. Every piece
of data from the user (registers, pointers, lengths) is potentially malicious.
The syscall handler must validate everything:

1. **Bounds-check the syscall number** — out-of-range a7 must not index past the table
2. **Validate file descriptors** — fd must refer to a real open file (for now, only fd=1 is valid)
3. **Validate lengths** — negative or absurdly large lengths must be rejected
4. **Validate user pointers via copyin** — never dereference a user-supplied VA directly; always walk the page table and check PTE_U

### What copyin prevents

Without copyin, a malicious user program could:
- Pass a VA in the kernel's address space → kernel reads its own secrets and sends them to console
- Pass a VA pointing to the trapframe (mapped without PTE_U) → same issue
- Pass an unmapped VA → kernel page faults in supervisor mode → panic (denial of service)

With copyin:
- VA checked against user page table → only pages with PTE_U are accessible
- Invalid/unmapped VA → returns -EFAULT → user gets an error, kernel continues

### The PTE_U check is load-bearing

The trapframe is mapped in the user page table (at VA `TRAPFRAME`) but **without
PTE_U**. This means user code cannot read/write it (hardware enforces this in
U-mode). copyin also rejects it — so even if user passes TRAPFRAME as a buffer
address, the kernel won't copy from it.

---

## What's Next

Round 6-2 implementation:
1. Create `include/syscall_num.h` and `include/errno.h`
2. Rename `exit`/`wait`/`kill` → `proc_exit`/`proc_wait`/`proc_kill`
3. Add `copyin()`/`copyout()` to vm.c
4. Create `kernel/syscall.c` with dispatch + sys_write + sys_exit
5. Wire into user_trap (replace print+kill with dispatch)
6. Update user program to print "hello world" and exit

---

## Quick Reference

### Syscall Numbers

| Number | Name | Linux RISC-V | Args | Returns |
|--------|------|-------------|------|---------|
| 1 | write | 64 | fd, buf, len | bytes written or -error |
| 2 | exit | 93 | status | never returns |

### Error Codes

| Code | Name | Meaning |
|------|------|---------|
| 9 | EBADF | Bad file descriptor |
| 14 | EFAULT | Bad user address |
| 22 | EINVAL | Invalid argument |
| 38 | ENOSYS | Syscall not implemented |

### Register Convention

| Register | Role |
|----------|------|
| a7 | Syscall number |
| a0 | Arg 1 / return value |
| a1 | Arg 2 |
| a2 | Arg 3 |
| a3 | Arg 4 |
| a4 | Arg 5 |
| a5 | Arg 6 |

### Control Flow

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
