# Lecture 6-1: User Address Space & Trampoline

> **Where we are**
>
> Phase 5 is complete. The kernel has processes with full lifecycle:
> creation, scheduling, sleeping, waking, exiting, and reaping. Kernel
> threads run and interleave via context switch. Spinlocks protect shared
> state. Wait queues let processes sleep until events occur. Init reaps
> zombies.
>
> But everything runs in supervisor mode. Every "process" is a kernel
> thread — it has full access to all hardware, all memory, all CSRs.
> There is no isolation. A buggy process can corrupt the page table,
> overwrite another process's stack, or disable interrupts and hang the
> system. There is no protection.
>
> Phase 6 introduces **user mode** — the hardware-enforced boundary
> between trusted kernel code and untrusted user programs. This first
> round builds the mechanism for crossing that boundary: the trampoline
> page, the trapframe, per-process user page tables, and the assembly
> glue that transitions between user and kernel execution.
>
> By the end of this round, a tiny hardcoded user program will execute
> in U-mode, issue an `ecall`, trap into the kernel, and the kernel will
> print its argument. The user/kernel boundary — the most important
> security boundary in any operating system — will be proven working.
>
> **What you will understand after this lecture:**
>
> - What U-mode is, what it restricts, and how to enter it (the sret trick)
> - The end-to-end trap flow: user code → user_vec → user_trap → user_trap_ret → user_ret → user code
> - The trapframe — layout, purpose, the sscratch trick
> - The trampoline page — why it exists, why it's mapped in both page tables
> - Per-process user page tables, address space layout, and TLB cost
> - `user_vec` / `user_ret` — the assembly that crosses the boundary
> - `user_trap` / `user_trap_ret` — the C functions that process user traps
> - Meltdown and why separate page tables are the right design
>
> **xv6 book coverage:** Chapter 3 section 3.6 (process address space),
> Chapter 4 sections 4.2–4.4 (traps from user space, calling system
> calls, system call arguments). We absorb 4.2 and the relevant parts
> of 3.6 fully. Section 4.3–4.4 (syscall dispatch) is previewed here but
> implemented in Round 6-2.

---

## Part 1: User Mode — What It Is and Why We Need It

### The privilege hierarchy (recap)

In Lecture 2-1, we introduced the three RISC-V privilege modes:

| Mode | Level | Who runs here | What it can do |
|------|-------|---------------|----------------|
| M-mode (Machine) | Highest | Firmware / our mini-SBI | Everything — full hardware control |
| S-mode (Supervisor) | Middle | Our kernel | Manage page tables, handle traps, access devices (via PMP permission) |
| U-mode (User) | Lowest | User programs | Only what the kernel allows — restricted memory, no privileged CSRs, no privileged instructions |

Until now, EVERYTHING in bobchouOS has run in S-mode. Our "processes"
are kernel threads — they share the kernel page table, can access any
CSR, can read/write any physical address. There is no protection
between them.

### What U-mode restricts

When the CPU is in U-mode, hardware enforces:

1. **Memory isolation**: only pages with `PTE_U` set are accessible.
   Accessing a page without PTE_U → immediate page fault (trap to
   S-mode). This means user code cannot read kernel memory, device
   registers, or other processes' pages.

2. **No privileged CSR access**: attempting `csrr`/`csrw` on S-mode
   CSRs (sstatus, satp, stvec, etc.) → illegal instruction trap. User
   code cannot modify page tables, disable interrupts, or change the
   trap vector. (A few read-only counters like `cycle` and `time` CAN
   be exposed to U-mode — controlled by the S-mode `scounteren`
   register. We won't use this for now.)

3. **No privileged instructions**: `sret`, `sfence.vma` — always trap
   from U-mode. (`wfi` may also trap depending on `mstatus.TW`; on our
   QEMU setup it does.) The only way to request kernel services is
   `ecall`.

4. **No device access** (by our page table policy): device registers
   are memory-mapped (UART at 0x10000000, PLIC at 0x0C000000, etc.).
   U-mode *could* access them if the page table mapped those addresses
   with PTE_U — but our user page tables deliberately don't. So user
   code that tries to read/write device memory gets a page fault. This
   is a kernel design choice, not a hardware rule.

5. **`ecall` is the ONLY voluntary way out**: user code that wants
   the kernel to do something (write to console, allocate memory, exit)
   must issue `ecall`, which traps into S-mode at the address in
   `stvec`. This is how system calls work.

   Importantly, `ecall` from U-mode traps to S-mode ONLY — user code
   cannot reach M-mode directly. Privilege levels are strict layers:

   ```
   U-mode ecall  →  S-mode  (scause = EXC_ECALL_U)
   S-mode ecall  →  M-mode  (mcause = EXC_ECALL_S)
   ```

   If a user program needs something that ultimately requires M-mode
   (e.g., setting the timer), the path is: user ecall → kernel syscall
   handler → kernel calls SBI (S-mode ecall) → M-mode handles it →
   returns to kernel → returns to user. The kernel is the gatekeeper;
   user code can never skip a privilege level.

### Why each process needs its own page table

In Phase 4, we built ONE kernel page table — identity-mapped, shared
by all kernel threads. That works because kernel threads are trusted
(they're all part of the kernel) and they share one address space.

User processes are different:

1. **Isolation**: process A must not be able to read process B's memory.
   If they shared a page table, any address A can access, B can too.
2. **Independent address spaces**: two programs can both use VA 0x1000
   for their code — they don't conflict because each has its own page
   table mapping 0x1000 to different physical pages.
3. **Kernel protection**: the user page table must NOT map kernel
   memory. If it did, user code (PTE_U pages) could still try to
   exploit hardware bugs to read kernel data.

So each user process gets its own page table. The kernel switches
`satp` each time it enters/exits user mode:

```
kernel PT (satp)   →   user PT (satp)   →   kernel PT (satp)
     │                      │                      │
  kernel runs         user code runs          kernel runs
  (S-mode)            (U-mode)                (S-mode)
```

The user page table contains:
- The process's own code and data (PTE_U set — user can access)
- A trampoline page (PTE_U NOT set — only accessible after trap to
  S-mode, needed for the page table switch)
- A trapframe page (PTE_U NOT set — same reason)
- Nothing else — no kernel code, no kernel data, no devices

The `PTE_U` flag is the key enforcement mechanism:

| PTE_U | U-mode access | S-mode access |
|-------|---------------|---------------|
| Set (1) | Allowed | **Fault** by default (unless `sstatus.SUM=1`) |
| Clear (0) | **Fault** — page fault trap | Allowed |

By default (`SUM=0`), S-mode cannot access pages with PTE_U set —
this prevents the kernel from accidentally dereferencing user pointers
without explicit copyin/copyout functions. We'll set `SUM=1` only when
we need to deliberately copy data between kernel and user space
(Round 6-2).

Pages with PTE_U=0 in the user page table are invisible to user code
but accessible to the kernel during the brief trampoline window (after
the trap switches to S-mode but before the page table switch). This
is how the trapframe (mapped without PTE_U) works — S-mode `user_vec`
can access it, but U-mode user code cannot.

### Why a user process needs TWO stacks

In Phase 5, each kernel thread had one kernel stack — allocated by
`kalloc()`, used for all its C call frames. That's simple because
kernel threads never leave S-mode.

A user process needs **two** stacks:

| Stack | Mapped in | PTE_U | Used when | Purpose |
|-------|-----------|-------|-----------|---------|
| User stack | User PT | Yes | Running in U-mode | Function calls, local variables in user code |
| Kernel stack | Kernel PT | No | Handling traps/syscalls in S-mode | C call frames for user_trap, syscall handlers, yield, sched |

Why not reuse the user stack for kernel work?

1. **Security**: user code controls its own stack pointer. A malicious
   program could set `sp` to point anywhere — into kernel memory, into
   another process's data. The kernel must never trust the user `sp`.

2. **Reliability**: user code might overflow its stack, corrupt it, or
   unmap it. The kernel needs a guaranteed-valid stack for trap handling.

3. **Isolation**: kernel call frames contain sensitive data (return
   addresses into kernel code, local variables with kernel pointers).
   Storing these on a user-accessible page would leak information.

The lifecycle of the two stacks:

```
User mode (U-mode):
    sp → user stack (in user PT, PTE_U)
    kernel stack exists but unused (empty, waiting)

Trap fires (→ S-mode):
    user sp saved to trapframe
    sp loaded from trapframe→kernel_sp → kernel stack
    kernel does its work using kernel stack

Return to user mode:
    user sp restored from trapframe
    sp → user stack again
    kernel stack empty again (ready for next trap)
```

This is why `kernel_sp` must be in the trapframe — it's how `user_vec`
finds the kernel stack after the trap. The user stack address is just
one of the 31 saved user registers (the user's `sp`).

### How do we get INTO U-mode?

The same trick we used in Phase 2 to get from M-mode into S-mode:
the **`sret` instruction**.

Recall: in `entry.S`, we set `mstatus.MPP = S-mode`, loaded our
kernel entry address into `mepc`, and executed `mret`. The hardware
restored the mode from MPP (→ S-mode) and jumped to mepc. We
"returned" to a place we'd never been — the mret trick.

The `sret` trick is identical, one level down:

| | M→S (Phase 2) | S→U (now) |
|---|---|---|
| Set previous-mode field | `mstatus.MPP = 01` (S-mode) | `sstatus.SPP = 0` (U-mode) |
| Set return address | `mepc = kmain` | `sepc = user program entry` |
| Execute | `mret` | `sret` |
| Effect | CPU enters S-mode at kmain | CPU enters U-mode at user code |

After `sret` with `SPP=0`, the CPU is in U-mode. All the restrictions
above are in effect. The user program runs until it hits `ecall` (or
an interrupt or fault fires) — at which point the hardware switches
back to S-mode and jumps to `stvec`.

---

## Part 2: The Big Picture — End-to-End Trap Flow

### Why kernel_vec doesn't work for user traps

Our current trap handler (`kernel_vec`) works beautifully for traps
that occur while the kernel is running. It relies on three assumptions:

1. **`sp` points to a valid kernel stack** — so `addi sp, sp, -256`
   pushes a save frame onto accessible memory
2. **`satp` points to the kernel page table** — so every memory access
   during the handler reaches the correct physical page
3. **`tp` holds the hart ID** — so `this_cpu()` works immediately

When a trap fires in **user mode**, NONE of these hold:

| Assumption | In kernel mode | In user mode |
|---|---|---|
| `sp` is kernel stack | Yes | No — `sp` is the *user* stack (different page table, different VA) |
| `satp` is kernel PT | Yes | No — `satp` points to the *user* page table |
| `tp` is hart ID | Yes | No — `tp` is whatever the user program put there |

If we just jumped to `kernel_vec` from user mode, the very first
instruction (`addi sp, sp, -256`) would push onto the *user* stack —
which is (a) the wrong place to save kernel state, and (b) potentially
unmapped or corrupted by a malicious program.

We need a completely different trap entry path for user mode. Here's
the full picture of what that path looks like — don't worry about the
details yet, we'll go through each piece in Parts 3–7:

### The complete flow

```
User program traps (ecall / timer interrupt / page fault / ...)
    │
    ├─ Hardware: mode U→S, PC→stvec (= TRAMPOLINE VA), save PC in sepc
    │
    ▼
user_vec (trampoline page, user PT still active)
    │  csrrw a0, sscratch, a0        → get trapframe pointer
    │  sd all regs to trapframe      → save user state
    │  ld kernel_satp, kernel_sp, user_trap, hartid
    │  csrw satp, kernel_satp        → switch to kernel PT
    │  sfence.vma
    │  jr t1                         → jump to user_trap
    │
    ▼
user_trap (kernel code, kernel PT, kernel stack)    ← DISPATCH ONLY
    │  csrw stvec, kernel_vec        → arm kernel trap handler
    │  save sepc to trapframe->epc
    │  dispatch: ecall → intr_on, syscall()
    │            interrupt → clear SSIP, set need_resched
    │            exception → set killed
    │  call user_trap_ret()
    │
    ▼
user_trap_ret (still kernel)                       ← SCHEDULING + RETURN
    │  if killed → exit(-1)
    │  if need_resched → yield()
    │  intr_off
    │  csrw stvec, trampoline        → arm user trap handler
    │  fill trapframe kernel fields
    │  set sstatus (clear SPP, set SPIE)
    │  csrw sepc, trapframe->epc
    │  call user_ret(TRAPFRAME, user_satp)
    │
    ▼
user_ret (trampoline page, kernel PT → user PT)
    │  csrw satp, user_satp          → switch to user PT
    │  sfence.vma
    │  csrw sscratch, TRAPFRAME VA   → reload for next trap
    │  ld all regs from trapframe    → restore user state
    │  sret                          → mode S→U, PC = sepc
    │
    ▼
User program resumes at next instruction
```

### The parallel between kernel and user trap paths

Both paths follow the same three-phase structure:

| Phase | Kernel path | User path |
|-------|-------------|-----------|
| Save + enter | `kernel_vec` (push regs on kstack) | `user_vec` (save regs to trapframe, switch PT) |
| Dispatch | `kernel_trap()` (identify cause, handle it) | `user_trap()` (identify cause, handle it) |
| Scheduling + return | `kernel_trap_ret()` (check killed/need_resched, yield, then return to kernel_vec which restores + sret) | `user_trap_ret()` (check killed/need_resched, yield, then set up CSRs and call user_ret which restores + sret) |

The key design principle: **dispatch and scheduling decisions are
separate concerns.** The dispatch function (`kernel_trap` / `user_trap`)
handles the cause and sets flags. The return function (`kernel_trap_ret`
/ `user_trap_ret`) makes scheduling decisions — yielding if preempted,
exiting if killed — before returning to the interrupted code.

### stvec timeline

`stvec` must point to the right handler at each phase. If a trap
fires while `stvec` points to the wrong handler, the system crashes.

| Phase | What's running | stvec points to | Who set it |
|-------|---------------|-----------------|------------|
| User code executing | User program (U-mode) | TRAMPOLINE (user_vec) | user_trap_ret |
| user_vec | Trampoline asm (S-mode) | TRAMPOLINE (user_vec) | (unchanged from above) |
| user_trap / syscall | Kernel C code (S-mode) | kernel_vec | user_trap (first line) |
| user_trap_ret | Kernel C code (S-mode) | TRAMPOLINE (user_vec) | user_trap_ret (sets it) |
| user_ret | Trampoline asm (S-mode) | TRAMPOLINE (user_vec) | (unchanged from above) |

**Why this matters**: if a timer interrupt fires during syscall
processing (the "user_trap / syscall" row), `stvec` points to
`kernel_vec` → the interrupt goes through the kernel trap path (saves
on kernel stack, calls `kernel_trap`, returns to exactly where the
syscall handler was). Clean, composable. If `stvec` still pointed to
the trampoline during kernel execution, it would re-enter `user_vec`,
which swaps `a0` with `sscratch` — but `sscratch` holds the user's
old `a0` (not a trapframe pointer), so the subsequent `sd`
instructions would write to an invalid address — crash.

**What about the other phases?** During `user_vec`, `user_ret`, and
`user_trap_ret`, interrupts are **disabled** — RISC-V automatically
clears `sstatus.SIE` on trap entry, and `user_trap_ret` explicitly
calls `intr_off()`. No trap can fire during these phases, so the
stvec value is irrelevant.

The only phase where a nested trap *can* arrive is during `user_trap`
/ syscall processing, where we explicitly re-enable interrupts with
`intr_on()`. Why enable them there? Compare the two trap paths:

- **Kernel traps** (`kernel_vec` → `kernel_trap` → `kernel_trap_ret`):
  brief — handle a timer tick (set `need_resched`), return. A few
  instructions. Interrupts stay off the whole time. No risk of
  starvation.

- **User syscalls** (`user_trap` → syscall handler): can be long. In
  later rounds, `read()` might wait for disk I/O, `wait()` might
  sleep for a child to exit. If interrupts stayed off during the
  entire syscall, the timer couldn't fire — no preemption, no
  scheduling for other processes. The system would freeze until the
  syscall completes.

So `user_trap` re-enables interrupts to allow preemption during
syscall processing. This is safe because `stvec` already points to
`kernel_vec` (set as user_trap's first action), and we're on the
kernel stack with the kernel page table — exactly the state
`kernel_vec` expects. The nested interrupt goes through `kernel_vec`
→ `kernel_trap` → sets `need_resched` → returns to exactly where the
syscall handler was. When the syscall finishes and `user_trap` calls
`user_trap_ret`, the scheduling check there sees `need_resched` and
calls `yield()`.

**How `intr_on()` interacts with `irqsave`/`irqrestore` in syscalls:**

The `intr_on()` sets the "baseline" interrupt state. Inside a syscall,
locks use `spin_lock_irqsave` / `spin_unlock_irqrestore`:

```
user_trap: intr_on()                         ← baseline: ON
  syscall → sys_read:
    [validate args, look up file]            ← ON — timer can preempt
    spin_lock_irqsave(&lock)                 ← saves ON, disables → OFF
    [critical section]                       ← OFF — atomic
    spin_unlock_irqrestore(&lock)            ← restores to ON
    [copy data to user buffer]               ← ON — timer can preempt
```

If we never called `intr_on()`, the baseline would stay OFF (from trap
entry), and every `irqrestore` would restore to OFF — interrupts
never come back on during the entire syscall, starving the timer.

**Why kernel traps don't need `intr_on()`:**

Our `kernel_trap` only handles brief events — timer tick (set a flag),
breakpoint (print + advance PC). A few instructions, never sleeps,
never does I/O. No risk of starving other processes. Keeping interrupts
off the entire time is simpler and safe.

> **What about long-running kernel work?** Linux supports "kernel
> preemption" — interrupts enabled during kernel execution, so the
> timer can preempt even kernel threads doing heavy computation. This
> requires careful design (no preemption while holding spinlocks).
> Linux also splits interrupt handlers into a "top half" (quick,
> interrupts off) and "bottom half" (longer work, interrupts on).
>
> We deliberately avoid this complexity. Our `kernel_trap` is always
> short. Long work (disk I/O, sleeping) only happens in syscall
> handlers — which run with interrupts on via `user_trap`'s
> `intr_on()`. In Phase 7 (disk driver), we'll use top/bottom half
> splitting, but only for the user-facing path.

### But wait — doesn't the scheduler run in S-mode?

Yes. `swtch()` is just a function call — it saves callee-saved
registers and restores another set. It **never changes privilege
level**. The scheduler runs in S-mode, calls `swtch()`, the process
resumes in S-mode. Always.

So how does a user process end up in U-mode? The answer: `swtch()`
resumes the process at its saved `context.ra`, which eventually calls
`user_trap_ret()` → `user_ret` → `sret`. The `sret` at the very end
is what drops to U-mode. Everything before it is S-mode kernel code:

```
scheduler (S-mode)
    │
    │ swtch() — just a function call, no privilege change
    ▼
process resumes (S-mode)
    │
    │ yield() returns → back inside user_trap_ret
    │ ... user_trap_ret continues: set up CSRs, call user_ret
    ▼
user_ret (S-mode, trampoline page)
    │
    │ sret  ← the ONE instruction that drops to U-mode
    ▼
user code (U-mode)
```

Compare with kernel threads, which never execute `sret`:

| Process type | After swtch resumes | Eventually does sret? |
|---|---|---|
| Kernel thread | Returns into thread function (S-mode forever) | No — never leaves S-mode |
| User process | yield() returns inside `user_trap_ret` → continues to `user_ret` → `sret` | Yes — drops to U-mode |

The difference is not in how the scheduler picks them — it's identical.
The difference is what happens AFTER swtch returns. Kernel threads
stay in S-mode. User processes take the extra step of `sret` to cross
into U-mode.

---

## Part 3: The Trapframe and sscratch

### The three trap handlers — why each works differently

We now have three trap handlers in bobchouOS, each with a different
strategy for saving registers. The key factor is: **is `sp` usable
when the trap fires?**

| | `m_vec` (M-mode) | `kernel_vec` (S-mode from kernel) | `user_vec` (S-mode from user) |
|---|---|---|---|
| When it fires | S-mode code ecalls/timer → M-mode | Kernel code interrupted in S-mode | User code traps to S-mode |
| `sp` usable? | No — sp is S-mode's, M-mode can't use it | **Yes** — sp is already a valid kernel stack | No — sp is user stack (wrong PT, untrusted) |
| Page table? | No paging in M-mode (irrelevant) | Kernel PT active (everything accessible) | User PT active (kernel stack inaccessible) |
| Strategy | `csrrw a0, mscratch, a0` → save 2 regs to scratch area | `addi sp, sp, -256` → push all regs onto kernel stack | `csrrw a0, sscratch, a0` → save 31 regs to trapframe |
| Needs scratch CSR? | Yes (`mscratch`) | No — sp works directly | Yes (`sscratch`) |
| Regs saved | Only 2 (t0, t1 — all handler touches) | All 31 (yield might switch to another process) | All 31 (must preserve user computation) |

**The pattern**: you need a scratch CSR when `sp` isn't yours.
`kernel_vec` is the easy case — the kernel stack is right there in
`sp`. The other two are the hard case — they enter from a "foreign"
context where `sp` doesn't belong to them.

### What makes this hard

The fundamental chicken-and-egg problem:

**To save registers, you need a free register to hold the destination
address. But all registers belong to the user program.**

You can't clobber any register — the user program's computation is in
those registers and must be restored exactly on return. You need to
save them somewhere, but to save them, you need an address register.
Circular.

### What the hardware gives us: sscratch

Same trick as `mscratch` in our mini-SBI (`m_vec.S`, Lecture 5-1
Part 4 — the redesigned version that replaced the Lecture 2-3 timer
handler). `sscratch` is the S-mode equivalent — one privilege level
down:

| | M-mode trap (mini-SBI) | S-mode trap from U-mode (now) |
|---|---|---|
| Problem | Need scratch reg in M-mode handler | Need scratch reg in S-mode handler |
| CSR | `mscratch` | `sscratch` |
| Pre-loaded with | Address of per-hart scratch area (16 bytes) | Address of per-process trapframe (full page) |
| First instruction | `csrrw a0, mscratch, a0` | `csrrw a0, sscratch, a0` |
| Accessible from | M-mode only | S-mode only (user can't touch it) |

The pattern is identical:
- Before entering the lower privilege mode, stash a useful pointer in
  the scratch CSR
- On trap entry, atomically swap it into a GP register
- Now you have one free register holding a known pointer

```asm
csrrw  a0, sscratch, a0
# Atomic swap: temp = sscratch; sscratch = a0; a0 = temp
# After: a0 = the value kernel left in sscratch (trapframe pointer)
#         sscratch = user's original a0 (safe, will save it later)
```

Now we have one free register (`a0`) holding a kernel-prepared pointer.
That pointer is the **trapframe** — a page of memory where we'll save
all 31 user registers.

### But what does a0 point to?

It points to the trapframe page, which must be mapped in the **user**
page table. Why? Because `satp` still points to the user page table
when this instruction executes — the hardware doesn't switch page
tables on trap. Any memory access goes through the user page table
until we explicitly switch `satp` in software.

This requirement shapes the entire design.

### The trapframe: M-mode scratch area, scaled up

Our mini-SBI (`m_vec.S`, Lecture 5-1 Part 4) uses the same pattern.
`mscratch` points to a 16-byte save area; the handler swaps `a0` with
`mscratch`, saves `t0`/`t1` into that area, does its work, restores,
and returns. Let's compare:

| | M-mode scratch (mini-SBI) | User trapframe (now) |
|---|---|---|
| Purpose | Save 2 regs so handler can work | Save ALL user regs + bootstrap kernel state |
| Size | 2 × 8 = 16 bytes (t0, t1) | 36 × 8 = 288 bytes (5 kernel fields + 31 regs) |
| Scope | Per-hart (one scratch area per CPU) | Per-process (one trapframe per user process) |
| Pointed to by | `mscratch` | `sscratch` |
| Mapped where | Physical address (no paging in M-mode) | Must be mapped in the *user* page table |
| Contains boot info? | No — just save slots | Yes — kernel_satp, kernel_sp, user_trap, hartid |
| Full register save? | No — only t0/t1 (handler is careful to use only those) | Yes — must preserve entire user computation |

The trapframe is the same idea — a pre-prepared save area accessed
via a scratch CSR — but scaled up because:
- We save ALL 31 registers (not just 2), since user state must be
  perfectly restored on return
- We need kernel bootstrap values (satp, sp, trap handler, hartid)
  because we're switching page tables — the M-mode handler didn't need
  this since M-mode has no paging
- It's per-process, not per-hart, because each user process has its
  own register state and its own kernel stack

### What the trapframe stores

The trapframe is a per-process page of memory that serves two roles:

1. **Save area for user registers** — `user_vec` dumps all 31 GP
   registers here on trap entry, `user_ret` restores them on trap return

2. **Pre-loaded kernel bootstrap values** — the kernel writes certain
   values into the trapframe *before* entering user mode, so that
   `user_vec` can read them to set up the kernel environment

### Layout

```
Offset  Field          Written by        Read by
──────  ─────────────  ────────────────  ──────────────────
0       kernel_satp    user_trap_ret()   user_vec (to switch PT)
8       kernel_sp      user_trap_ret()   user_vec (to load kstack)
16      user_trap      user_trap_ret()   user_vec (to jump to C handler)
24      hartid         user_trap_ret()   user_vec (to reload tp)
32      epc            user_trap()       user_trap_ret() (to set sepc)
───── user registers (saved by user_vec, restored by user_ret) ─────
40       x1              # ra
48       x2              # sp
56       x3              # gp
64       x4              # tp
72       x5              # t0
80       x6              # t1
88       x7              # t2
96       x8              # s0/fp
104      x9              # s1
112      x10             # a0
120      x11             # a1
128      x12             # a2
136      x13             # a3
144      x14             # a4
152      x15             # a5
160      x16             # a6
168      x17             # a7
176      x18             # s2
184      x19             # s3
192      x20             # s4
200      x21             # s5
208      x22             # s6
216      x23             # s7
224      x24             # s8
232      x25             # s9
240      x26             # s10
248      x27             # s11
256      x28             # t3
264      x29             # t4
272      x30             # t5
280      x31             # t6
```

Total: 5 kernel fields + 31 registers = 36 × 8 = 288 bytes. Easily
fits in one 4096-byte page.

### Why kernel metadata lives IN the trapframe

After the `csrrw` swap, `a0` points to the trapframe. That's the
**only** thing the code knows. It can't access global variables (they
might not be mapped in the user page table). It can't call functions
(it doesn't know where the kernel is). It can only load from offsets
relative to `a0`.

So everything `user_vec` needs to bootstrap into the kernel must be
stored at known offsets in the trapframe:

- **kernel_satp** — the kernel page table's SATP value, so we can
  switch to it
- **kernel_sp** — this process's kernel stack pointer, so we have a
  stack for C code
- **user_trap** — the address of `user_trap()` in the kernel, so we
  know where to jump after switching page tables
- **hartid** — the hart ID to load into `tp`, so `this_cpu()`
  works

These are written by `user_trap_ret()` each time before returning to
user mode. They don't change between traps for a given process (the
kernel stack and page table are fixed), but writing them fresh each
time is simple and correct.

### Why epc is in the trapframe

When a trap fires, the hardware saves the user's program counter into
`sepc` (the S-mode Exception Program Counter — the PC of the
instruction that was interrupted). But `user_trap_ret()` might call
`yield()` (on a timer preemption), which switches to another process.
That other process might trap from user space too, overwriting `sepc`.
When we eventually switch back, the original `sepc` is gone.

Solution: `user_trap()` copies `sepc` into `trapframe->epc` early.
When returning, `user_trap_ret()` writes `trapframe->epc` back to
`sepc`. The trapframe is per-process, so each process's saved PC is
safe.

### The trapframe in the address space

The trapframe page is mapped at a fixed virtual address near the top
of the user address space:

```
TRAPFRAME = MAX_VA - 2 * PG_SIZE
```

It is mapped **without PTE_U** — meaning:
- Supervisor-mode code (user_vec, running after the trap) can access it
- User-mode code cannot read or write it (hardware fault if attempted)

This is important for security — user code must not be able to tamper
with `kernel_satp` or `kernel_sp` in the trapframe. The trap itself
switches to S-mode (the hardware guarantees this), so by the time
`user_vec` executes, the access is legal.

The kernel also accesses the trapframe through `p->trapframe` — a
pointer to the physical address of the page (e.g., 0x8050_0000),
which the kernel page table identity-maps. Two views of the same
physical page, accessed at different VAs:

| Who | VA used | Page table |
|-----|---------|------------|
| `user_vec` / `user_ret` (in trampoline) | `TRAPFRAME` (high VA, near MAX_VA) | User PT |
| `user_trap()` / `user_trap_ret()` (in kernel) | `p->trapframe` (low VA = PA, in DRAM range) | Kernel PT (identity-mapped) |

**Unlike the trampoline, the trapframe doesn't need the same VA in
both page tables.** The trampoline needs a matching VA because code
*executes on it* during the `satp` switch — the PC must be valid in
both PTs simultaneously (Part 4 explains this in detail). The
trapframe is only ever accessed by one PT at a time: `user_vec` reads
it before the switch (user PT), `user_trap` reads it after the switch
(kernel PT). No overlap.

Why use a fixed high VA (`TRAPFRAME`) in the user PT instead of the
identity-mapped physical address? Because `TRAPFRAME` is a
compile-time constant — `user_trap_ret` can pass it to `user_ret`
as an immediate, and `user_ret` writes it into `sscratch`. If we used
the physical address (different per process), `user_ret` would need
to compute it dynamically in assembly — more complex for no benefit.

### The alternative: Linux's kernel-stack approach

> Our design (following xv6) saves all 31 user registers into the
> trapframe **before** switching page tables. Linux takes a different
> approach: switch page tables first with minimal register saves, then
> dump all regs onto the kernel stack. This is worth understanding as a
> comparison.
>
> **Linux's per-CPU trampoline data:**
>
> Instead of a per-process trapframe, Linux maps a small per-CPU struct
> in the user page table (in the same page as the trampoline code):
>
> ```c
> // Per-CPU, mapped in user PT (no PTE_U). ~40 bytes.
> struct trampoline_data {
>     uint64 scratch;         // save slot for 1 scratch register
>     uint64 kernel_satp;     // kernel page table SATP value
>     uint64 kernel_sp;       // current process's kernel stack top
>     uint64 user_trap;       // address of C trap handler
>     uint64 hartid;          // hart ID (to restore tp)
> };
> ```
>
> This looks like our trapframe's first 5 kernel fields — but without
> the 31 user register save slots. And it's per-CPU, not per-process.
> The scheduler updates `kernel_sp` each time it switches to a new
> process.
>
> **Linux's trap entry sequence:**
>
> ```asm
> user_vec:
>     csrrw t0, sscratch, t0       # t0 = &trampoline_data
>     sd    sp, 0(t0)              # save user sp (1 reg, user PT)
>     ld    sp, 16(t0)             # sp = kernel_sp (user PT)
>     ld    t1, 8(t0)              # t1 = kernel_satp (user PT)
>     csrw  satp, t1               # ← switch PT HERE
>     sfence.vma
>     # --- kernel PT active, kernel stack accessible ---
>     addi  sp, sp, -248           # push frame for all user regs
>     sd    ra,  0(sp)             # save user regs to kernel stack
>     sd    gp,  8(sp)
>     ...                          # save remaining 28 regs
>     csrr  t1, sscratch           # recover user t0
>     sd    t1, xx(sp)
>     ld    t1, 0(t0)              # recover user sp (from scratch slot)
>     sd    t1, yy(sp)
>     # load tp, jump to C handler...
> ```
>
> **Where user regs end up — the kernel stack:**
>
> Each process's kernel stack has a "reserved zone" at the top
> (`struct pt_regs`) where all 31 user registers are saved. Below
> that is the normal C call stack:
>
> ```
> Kernel stack (Linux):            Kernel stack (bobchouOS):
> ┌─────────────────────┐         ┌─────────────────────┐
> │ struct pt_regs      │         │                     │
> │ (31 user regs)      │         │ (purely C frames)   │
> │ ~ 248 bytes         │         │                     │
> ├─────────────────────┤         ├─────────────────────┤
> │ user_trap() frame   │         │ user_trap() frame   │
> │ yield() frame       │         │ yield() frame       │
> │ ...                 │         │ ...                 │
> └─────────────────────┘         └─────────────────────┘
>                                  User regs are in trapframe
>                                  (separate page, not here)
> ```
>
> **Accessing user regs from C (Linux):**
>
> ```c
> // Linux: compute from stack top, or passed as argument
> void user_trap(struct pt_regs *regs) {
>     uint64 syscall_num = regs->a7;
>     regs->a0 = return_value;
> }
>
> // bobchouOS: via proc struct
> struct proc *p = this_proc();
> uint64 syscall_num = p->trapframe->a7;
> p->trapframe->a0 = return_value;
> ```
>
> **Summary of differences:**
>
> | | xv6/bobchouOS (our choice) | Linux KPTI |
> |---|---|---|
> | Save regs, then switch PT | Yes | No — switch PT first, then save |
> | User regs stored in | Trapframe (separate page per proc) | Kernel stack top (per proc) |
> | Bootstrap data in user PT | Per-process trapframe (full page) | Per-CPU struct (~40 bytes) |
> | Pages in user PT | 1 shared (trampoline) + 1 per proc (trapframe) | 1 per CPU (trampoline code + data) |
> | Kernel stack contains | Only C call frames | User regs + C call frames |
> | Scheduler must update | Nothing extra | Per-CPU `kernel_sp` |
> | Memory cost (100 procs) | 100 trapframe pages | 0 extra (regs on existing kstack) |
>
> **Why we use the xv6 approach:**
>
> - Simpler trampoline assembly — save everything via one pointer (`a0`)
>   before doing anything else
> - Clear conceptual separation — user register state has a single
>   well-defined home (`p->trapframe`), not mixed into the kernel stack
> - Easier to understand during context switch — the trapframe persists
>   unchanged while the kernel stack unwinds and rewinds
> - The memory cost (one extra page per process) is negligible for a
>   teaching OS
>
> Linux's approach optimizes for minimal exposure in the user page
> table (security) and avoids per-process page allocations for the
> trapframe (memory). For a production kernel running thousands of
> processes on hardware vulnerable to speculative side-channel attacks,
> these tradeoffs matter. For bobchouOS, clarity wins.

---

## Part 4: The Trampoline

### The page table switch problem

We mentioned in Part 3 that the trampoline needs the same VA in both
page tables. Here's why in detail.

After `user_vec` saves all user registers and loads `kernel_satp` from
the trapframe, it must switch page tables:

```asm
csrw  satp, kernel_satp_value
sfence.vma
# ← the next instruction is fetched using the NEW page table
```

The instruction AFTER `csrw satp` will be fetched using the **new**
page table. If the code's current virtual address doesn't exist in
the new page table, the CPU immediately faults.

We're executing `user_vec` at some VA. After switching to the kernel
page table, that same VA must still be valid — otherwise the CPU can't
fetch the next instruction.

### The solution: the trampoline page

The **trampoline** is a page of code containing `user_vec` and
`user_ret`. It's mapped at the same virtual address in BOTH page
tables:

```
User page table:    VA TRAMPOLINE → PA of trampoline page
Kernel page table:  VA TRAMPOLINE → PA of trampoline page (same!)
```

When `user_vec` switches `satp` from user→kernel, the PC doesn't
change — it's still pointing into the trampoline VA. The new (kernel)
page table has the trampoline at the same VA, so instruction fetch
continues seamlessly.

The same logic applies in reverse: `user_ret` switches `satp` from
kernel→user page table. The trampoline is mapped at the same VA in
the user PT, so execution continues seamlessly into the register
restore sequence.

### The name "trampoline"

A trampoline is something you **bounce off of** to get somewhere else.
You're not trying to stay on it — it's a transitional surface.

The trampoline code is exactly that: transitional. It runs during the
brief moment when you're switching between page tables. You land on it
from one world (user), bounce, and land in the other world (kernel).

### Trampoline address and permissions

```
TRAMPOLINE = MAX_VA - PG_SIZE    (one page, the highest page in the VA space)
```

Permissions: **R-X, no PTE_U**

- R-X: the CPU needs to execute it, and `user_ret` does loads from it
  (reading trapframe offsets). Actually — the loads come from the
  trapframe page (at TRAPFRAME), not the trampoline page. The
  trampoline itself only needs X. We add R for convenience (no harm,
  and some hardware requires R with X).
- No PTE_U: user code cannot jump to or read the trampoline directly.
  The CPU only reaches the trampoline via the trap mechanism (which
  switches to S-mode first).

### The physical address: a linker section

The kernel needs to know the trampoline's physical address so it can
map it into both page tables. We achieve this by placing the trampoline
code in its own linker section, page-aligned:

```ld
    . = ALIGN(4096);

    .text.trampoline : {
        *(.text.trampoline)
    } :text

    . = ALIGN(4096);
```

The trampoline's physical address is known via the `.globl trampoline`
label at the top of `trampoline.S` (declared in C as
`extern char trampoline[]`). The page-alignment before and after
ensures the trampoline occupies exactly one page (it won't share a
page with other kernel code). `vm_create_kernel_pt()` maps it at VA
`TRAMPOLINE`, and `proc_pagetable()` maps it at the same VA in each
user page table.

---

## Part 5: Address Space, Page Tables & Performance

### User process virtual address space

```
0x80_0000_0000  (MAX_VA = 2^39)
    ┌────────────────────────────────┐
    │ TRAMPOLINE (1 page, R-X)       │ MAX_VA - PG_SIZE
    ├────────────────────────────────┤
    │ TRAPFRAME  (1 page, R-W)       │ MAX_VA - 2*PG_SIZE
    ├────────────────────────────────┤
    │                                │
    │     (unmapped)                 │
    │                                │
    ├────────────────────────────────┤
    │ user stack  (1 page)           │ grows down within the page
    ├────────────────────────────────┤
    │ guard page  (unmapped)         │ stack overflow → page fault
    ├────────────────────────────────┤
    │ user text + data               │ starts at VA 0x1000
    ├────────────────────────────────┤
    │ (unmapped — NULL deref zone)   │ VA 0x0000 – 0x0FFF
    └────────────────────────────────┘
0x0000_0000_0000
```

Key design decisions:

| Choice | Value | Rationale |
|--------|-------|-----------|
| Text start | 0x1000 | Page 0 unmapped → NULL dereferences fault immediately |
| Guard page | 1 page between text and stack | Detects simple stack overflow |
| Stack | 1 page (4 KB) | Sufficient for small programs; demand-paged growth with guard page as hard limit in Round 6-4 (Linux defaults to 8 MB) |
| Trapframe VA | MAX_VA − 2×PG_SIZE | Fixed location so `sscratch` always holds the same value |
| Trampoline VA | MAX_VA − PG_SIZE | Top of VA space — same in every process |

### Kernel page table additions

The kernel page table (which we already built in Phase 4) gains one
new entry — and it's special:

```c
// In vm_create_kernel_pt() — the ONE non-identity mapping:
kvm_map(TRAMPOLINE, (uint64)trampoline, PG_SIZE, PTE_R | PTE_X);
```

This is the **only non-identity mapping** in the entire kernel page
table. Every other entry maps VA = PA (UART, PLIC, kernel text, DRAM).
The trampoline is different: VA is near MAX_VA, PA is in the kernel
text region. It must live at this high VA so that `user_vec`/`user_ret`
(executing at this VA) can survive the `satp` switch.

**What about the trapframe?** No kernel PT entry needed. The trapframe
page is allocated by `kalloc()` at runtime — it lands somewhere in
DRAM (between `_text_end` and `PHYS_STOP`). That range is already
identity-mapped by the existing `kvm_map(_text_end, ..., PHYS_STOP)`
call. So the kernel accesses `p->trapframe` (the physical address)
directly — it's already covered.

| Page | Kernel PT mapping | How |
|------|---|---|
| Trampoline | Non-identity: VA `TRAMPOLINE` → PA `trampoline` | Explicit new `kvm_map` (the only non-identity entry) |
| Trapframe | Identity: VA = PA (in DRAM) | Implicitly covered by `_text_end..PHYS_STOP` range |

### proc_pagetable() — creating the user page table

When a user process is created, we build its page table:

```c
pte_t *proc_pagetable(struct proc *p) {
    pte_t *pt;

    // Allocate root page table page (kalloc returns zeroed)
    if (!(pt = (pte_t *)kalloc()))
        return NULL;

    // Map trampoline at the top of the address space.
    // Same physical page as kernel's trampoline mapping.
    // R-X, no PTE_U — user code can't access it, but S-mode can.
    if (map_pages(pt, TRAMPOLINE, PG_SIZE,
                  (uint64)trampoline, PTE_R | PTE_X) < 0) {
        // cleanup...
        return NULL;
    }

    // Map this proc's trapframe page.
    // R-W, no PTE_U — user_vec (running in S-mode) writes here.
    if (map_pages(pt, TRAPFRAME, PG_SIZE,
                  (uint64)p->trapframe, PTE_R | PTE_W) < 0) {
        // cleanup...
        return NULL;
    }

    return pt;
}
```

Note: `p->trapframe` is the **physical address** of the trapframe page
(allocated via `kalloc()`). The mapping creates a virtual address
(`TRAPFRAME` = MAX_VA − 2*PG_SIZE) that resolves to that physical page
when the user page table is active. We use `pte_t *` as the page table
type.

### The test process — proving it works

For this round, we create a single test process with a hardcoded
user program:

```c
void proc_create_user_test(void) {
    struct proc *p = alloc_proc();

    // Allocate trapframe
    p->trapframe = (struct trapframe *)kalloc();

    // Build user page table
    p->pagetable = proc_pagetable(p);

    // Allocate a page for user code, copy test program into it
    void *code_page = kalloc();
    memmove(code_page, test_user_bin, test_user_bin_size);

    // Map at VA 0x1000 with user + execute permissions
    map_pages(p->pagetable, 0x1000, PG_SIZE,
              (uint64)code_page, PTE_R | PTE_X | PTE_U);

    // Allocate and map user stack
    void *stack_page = kalloc();
    uint64 stack_va = 0x3000;  // text at 0x1000, guard at 0x2000, stack at 0x3000
    map_pages(p->pagetable, stack_va, PG_SIZE,
              (uint64)stack_page, PTE_R | PTE_W | PTE_U);

    // Set up trapframe for first entry to user mode
    memset(p->trapframe, 0, PG_SIZE);
    p->trapframe->epc = 0x1000;            // start at beginning of user code
    p->trapframe->sp = stack_va + PG_SIZE; // stack grows down from top of page

    // Set process size
    p->sz = stack_va + PG_SIZE;

    // Mark runnable — scheduler will pick it up
    // (details depend on how we integrate with proc_create)
}
```

### The first trip to user mode

The process's `context.ra` is set to point to a function that calls
`user_trap_ret()`. When the scheduler first switches to this process,
it "returns" to that function, which sets up stvec/sscratch/sstatus/
sepc and calls `user_ret`. `user_ret` does `sret` — and we're in user
mode for the first time.

This is a one-time trick. After the first trap back, the process
re-enters user mode via the normal path: `user_trap` (dispatch) →
`user_trap_ret` (scheduling decisions + return to user).

### Meltdown and why we don't map the kernel in user page tables

> In 2018, security researchers discovered **Meltdown** — a hardware
> vulnerability in Intel CPUs (and some ARM chips). Modern CPUs execute
> instructions **speculatively**: they guess what comes next and start
> computing before confirming permissions.
>
> The attack exploited this: a user program could read kernel memory
> speculatively — the CPU would fault eventually, but the speculated
> data left traces in the CPU cache. By measuring cache access times,
> an attacker could reconstruct kernel memory byte by byte.
>
> **The precondition**: the kernel must be *mapped* in the user page
> table. The PTE permission bits say "supervisor only" — but speculative
> execution ignores permissions until the instruction retires. If the
> mapping exists at all, the CPU can speculatively load the data.
>
> **Before Meltdown**, most production OSes (Linux, Windows, macOS)
> mapped the entire kernel into every process's page table for speed.
> Trapping into the kernel required no page table switch — just a
> privilege escalation. Fast, convenient.
>
> **After Meltdown**, these OSes had to retrofit **Kernel Page Table
> Isolation (KPTI)**: strip kernel mappings from user page tables and
> add page table switches on every user↔kernel transition. Linux's KPTI
> is essentially the same design as xv6's trampoline — they added it
> as an emergency patch, with a ~5% syscall performance penalty.
>
> **Our position**: bobchouOS never maps the kernel in user page tables.
> We do the trampoline + page table switch from the start. This means:
> - We're immune to Meltdown by construction
> - We pay the performance cost of two `satp` writes per trap (one in,
>   one out)
> - On real hardware, ASIDs (Address Space IDs) mitigate this cost by
>   avoiding full TLB flushes — a Phase 9 optimization topic
>
> **The historical irony**: xv6's "simple teaching design" (separate
> page tables, trampoline) was accidentally the **secure** design all
> along. Production kernels optimized for speed, got burned by hardware
> bugs, and had to converge on what xv6 always did.
>
> | Era | Approach | Security | Perf |
> |-----|----------|----------|------|
> | Pre-2018 | Kernel mapped in user PT, no PT switch | Meltdown-vulnerable | Fast traps |
> | Post-2018 (KPTI) | Kernel unmapped, trampoline, PT switch | Meltdown-safe | ~5% syscall overhead |
> | xv6 / bobchouOS | Same as KPTI, from day one | Meltdown-safe | Same overhead (irrelevant for us) |

### The TLB flush cost

Every page table switch (`csrw satp`) must be followed by
`sfence.vma` to flush the TLB. Otherwise the hardware might use stale
translations from the previous page table. That's **two flushes per
user trap**:

```
User mode (user PT in satp)
    │ trap
    ▼
user_vec: csrw satp, kernel_satp → sfence.vma      ← flush 1
    │ kernel handles trap
    ▼
user_ret: csrw satp, user_satp → sfence.vma        ← flush 2
    │ sret
    ▼
User mode again
```

After each flush, the TLB is cold — every memory access walks the
page table in hardware until the TLB warms back up. For syscall-heavy
workloads (thousands of read/write calls per second), this dominates.

**What about process switches?** If the scheduler picks a different
user process, no *extra* flush is needed — the flush already happens
in `user_ret` when loading the new process's user PT. If the scheduler
picks a **kernel thread** instead, there's no user PT switch at all —
kernel threads run entirely in S-mode on the kernel PT (which stayed
in `satp` since `user_vec` loaded it). No flush.

**The ASID optimization** (recap from Lecture 4-1, Part 5): real
hardware avoids this cost using ASIDs — a 16-bit tag in `satp` that
identifies which address space the TLB entries belong to. Each TLB
entry is tagged with the ASID that created it. On page table switch,
you write a new ASID into `satp` — the hardware only matches entries
with the *current* ASID, so old entries from other processes are
invisible without being flushed:

```
Without ASID (us, xv6):
    csrw satp, new_value
    sfence.vma             ← must flush (old entries could match new VAs)

With ASID (Linux, real hardware):
    csrw satp, new_value   ← ASID field differs → old entries auto-invisible
    (no sfence.vma needed unless ASIDs are recycled)
```

We set ASID=0 always and flush on every switch — correct but slow on
real hardware. On QEMU, there's no real TLB, so the performance
difference is zero. ASID support is a Phase 9 optimization topic.

### Estimating the cost

Let's put concrete numbers on this. Assumptions (same order of
magnitude as Lecture 4-1, Part 6):

| Parameter | Value | Source |
|---|---|---|
| TLB hit | ~1 cycle | hardware lookup |
| TLB miss, PTE in L1 | ~10–15 cycles | 3-level walk, cached |
| TLB miss, PTE in L2 | ~30–50 cycles | 3-level walk, warm |
| TLB miss, PTE in DRAM | ~200+ cycles | 3-level walk, cold |
| Typical TLB entries | 64–1024 | depending on CPU |
| CPU clock | ~1–3 GHz | modern cores |

**Cost of one `sfence.vma` (full flush):**

The flush itself is fast (~1–5 cycles to issue). The real cost is
the TLB misses AFTER it — every subsequent memory access must walk
the page table until the TLB warms back up.

A typical syscall touches ~20–50 distinct pages during kernel
execution (kernel code, kernel stack, proc struct, trap handler, etc.).
Each of these misses the TLB after a flush:

```
Warm TLB (no flush):   50 accesses × 1 cycle  = ~50 cycles
Cold TLB (after flush): 50 accesses × 15 cycles = ~750 cycles
                                                   (assuming L1-cached PTEs)
```

**Cost per syscall (two flushes — entry and exit):**

```
Extra cost = 2 × (50 misses × ~15 cycles) = ~1500 cycles
At 1 GHz: ~1500 ns = 1.5 µs per syscall
```

**Impact on syscall-heavy workloads:**

A simple `read()` syscall without TLB flushing: ~1–2 µs (kernel
work). With two flushes: ~3–4 µs. That's roughly a **50–100%
overhead** on fast syscalls.

For workloads doing 100,000 syscalls/second:
```
Extra time = 100,000 × 1.5 µs = 150 ms/second = 15% overhead
```

This matches the ~5–15% overhead reported for Linux KPTI in
real-world benchmarks. The exact number depends on workload (I/O
heavy = more syscalls = more pain).

**Why we don't care (on QEMU):**

QEMU is an interpreter — each instruction is 100–1000× slower than
real hardware. TLB simulation doesn't model warm/cold effects. The
`sfence.vma` is essentially a no-op in QEMU's emulation. Our syscalls
are dominated by emulation overhead, not TLB misses.

On real hardware, this cost motivates ASIDs (Phase 9).

---

## Part 6: user_vec and user_ret — The Assembly

### user_vec: entering the kernel from user mode

When a trap fires while executing in user mode (ecall, interrupt, page
fault), the hardware does three things:

1. Sets `sstatus.SPP = 0` (records that we came from U-mode)
2. Saves the PC into `sepc`
3. Jumps to the address in `stvec` — which we set to the trampoline VA

Then `user_vec` executes (in S-mode, with user page table still active):

```asm
.section .text.trampoline
.globl user_vec
.align 4
user_vec:
    #
    # Trap from user space.
    # sscratch holds the TRAPFRAME VA (set by user_trap_ret before sret).
    # Swap a0 and sscratch — now a0 = trapframe ptr, sscratch = user a0.
    #
    csrrw a0, sscratch, a0

    #
    # Save all 31 user GP registers into the trapframe.
    # a0 (x10) currently points to the trapframe. The user's original a0
    # is in sscratch — we'll save it after the others.
    #
    sd x1,   40(a0)     # ra
    sd x2,   48(a0)     # sp
    sd x3,   56(a0)     # gp
    sd x4,   64(a0)     # tp
    sd x5,   72(a0)     # t0
    sd x6,   80(a0)     # t1
    sd x7,   88(a0)     # t2
    sd x8,   96(a0)     # s0/fp
    sd x9,  104(a0)     # s1
    # (x10/a0 is saved later — it's in sscratch right now)
    sd x11, 120(a0)     # a1
    sd x12, 128(a0)     # a2
    sd x13, 136(a0)     # a3
    sd x14, 144(a0)     # a4
    sd x15, 152(a0)     # a5
    sd x16, 160(a0)     # a6
    sd x17, 168(a0)     # a7
    sd x18, 176(a0)     # s2
    sd x19, 184(a0)     # s3
    sd x20, 192(a0)     # s4
    sd x21, 200(a0)     # s5
    sd x22, 208(a0)     # s6
    sd x23, 216(a0)     # s7
    sd x24, 224(a0)     # s8
    sd x25, 232(a0)     # s9
    sd x26, 240(a0)     # s10
    sd x27, 248(a0)     # s11
    sd x28, 256(a0)     # t3
    sd x29, 264(a0)     # t4
    sd x30, 272(a0)     # t5
    sd x31, 280(a0)     # t6

    # Save user a0 (currently in sscratch) into its trapframe slot
    csrr t0, sscratch
    sd   t0, 112(a0)    # x10/a0

    #
    # Load kernel state from the trapframe.
    # These were written by user_trap_ret() before we entered user mode.
    #
    ld sp,  8(a0)       # kernel_sp → sp (now we have a kernel stack)
    ld tp, 24(a0)       # hartid → tp (now this_cpu() works)
    ld t1, 16(a0)       # user_trap → t1 (C trap handler to jump to)
    ld t0,  0(a0)       # kernel_satp → t0

    #
    # Switch to kernel page table.
    # After this, we can only access memory mapped in the kernel PT.
    # The trampoline page is mapped at the same VA in both PTs,
    # so the next instruction fetch succeeds.
    #
    csrw satp, t0
    sfence.vma zero, zero

    #
    # Jump to user_trap (the C handler).
    # We're now executing with: kernel page table, kernel stack,
    # correct tp. Just like any kernel code.
    #
    jr t1
```

### Key observations (user_vec)

1. **Register discipline**: `a0` is repurposed immediately (it's our
   only working register). All other regs are saved before being
   clobbered. User `a0` is saved last (from `sscratch`).

   Why `a0` and not some other register (like `t0` in our `m_vec`)?
   Any register works — `csrrw t0, sscratch, t0` would be equally
   valid. `a0` is the conventional choice across xv6 and RISC-V OS
   literature; it makes no functional difference since we must save
   all 31 registers regardless of which one we use as the base pointer.

2. **Order matters**: we load `kernel_sp` into `sp` BEFORE switching
   page tables. All loads here use `a0` (trapframe) as base, and the
   trapframe IS mapped in the user PT (at the TRAPFRAME VA). So all
   these loads work fine before the `satp` switch. After the switch,
   we immediately jump to `user_trap` — which uses `sp` (now the
   kernel stack, mapped in the kernel PT). Everything is consistent.

3. **sfence.vma**: flushes the TLB so the hardware page table walker
   sees the new `satp` value. Without this, stale TLB entries from the
   user page table could cause incorrect translations.

4. **jr t1 (not call)**: we jump directly, no `call`. The return from
   `user_trap` will go through `user_trap_ret()` → `user_ret`, not back
   here. This is a one-way trip.

### user_ret: returning to user mode

`user_trap_ret()` (a C function) prepares everything, then calls
`user_ret`. The call happens while we're still on the kernel page
table and kernel stack — but `user_ret` lives in the trampoline page
(mapped at the same VA in both page tables), so it can switch safely.

`user_trap_ret()` passes two arguments:
- `a0` = the TRAPFRAME virtual address (base pointer — same register as user_vec)
- `a1` = the user page table's SATP value

```asm
.globl user_ret
user_ret:
    #
    # Switch to user page table.
    # After this, only the trampoline + trapframe are accessible.
    #
    csrw satp, a1
    sfence.vma zero, zero

    #
    # Restore user registers from the trapframe.
    # a0 = TRAPFRAME VA (passed by user_trap_ret) — same base register
    # as user_vec uses for saving. Symmetric.
    #
    # First, stash the trapframe address in sscratch for next trap.
    #
    csrw sscratch, a0     # sscratch = TRAPFRAME VA (ready for next trap)

    # Restore all user registers from trapframe (using a0 as base)
    ld x1,   40(a0)     # ra
    ld x2,   48(a0)     # sp
    ld x3,   56(a0)     # gp
    ld x4,   64(a0)     # tp
    ld x5,   72(a0)     # t0
    ld x6,   80(a0)     # t1
    ld x7,   88(a0)     # t2
    ld x8,   96(a0)     # s0/fp
    ld x9,  104(a0)     # s1
    # (x10/a0 restored last — we're still using it as base pointer)
    ld x11, 120(a0)     # a1
    ld x12, 128(a0)     # a2
    ld x13, 136(a0)     # a3
    ld x14, 144(a0)     # a4
    ld x15, 152(a0)     # a5
    ld x16, 160(a0)     # a6
    ld x17, 168(a0)     # a7
    ld x18, 176(a0)     # s2
    ld x19, 184(a0)     # s3
    ld x20, 192(a0)     # s4
    ld x21, 200(a0)     # s5
    ld x22, 208(a0)     # s6
    ld x23, 216(a0)     # s7
    ld x24, 224(a0)     # s8
    ld x25, 232(a0)     # s9
    ld x26, 240(a0)     # s10
    ld x27, 248(a0)     # s11
    ld x28, 256(a0)     # t3
    ld x29, 264(a0)     # t4
    ld x30, 272(a0)     # t5
    ld x31, 280(a0)     # t6

    # Finally restore x10/a0 itself
    ld x10, 112(a0)     # a0

    #
    # Return to user mode.
    # sepc was set by user_trap_ret() to the user PC.
    # sstatus.SPP was cleared to 0 (return to U-mode).
    # sret: pc = sepc, mode = U.
    #
    sret
```

### Key observations (user_ret)

1. **Symmetric base register**: `user_vec` saves via `a0`, `user_ret`
   restores via `a0`. Same register, same offsets, mirror image. The
   `user_satp` value is passed in `a1` — used once for `csrw satp`
   and then discarded.

2. **sscratch reload**: we write TRAPFRAME VA (from `a0`) into
   `sscratch` before restoring registers. Next time a trap fires,
   `user_vec` swaps `a0` with `sscratch` and gets the trapframe
   pointer back.

3. **a0 restored last**: just like `a0` was saved last in `user_vec`
   (because it was being used as the base pointer), `a0` is restored
   last here for the same reason. Perfect symmetry.

4. **sret**: the magic instruction. It reads `sepc` → sets PC to that
   value. It reads `sstatus.SPP` → switches to U-mode (because we
   cleared SPP to 0). It also sets `sstatus.SIE = sstatus.SPIE`
   (re-enabling interrupts if they were enabled before the trap).

---

## Part 7: user_trap and user_trap_ret — The C Side

### user_trap(): dispatch only

After `user_vec` jumps to `user_trap()`, we're fully in the kernel:
kernel page table active, kernel stack loaded, tp = hart ID. This
function's sole job is to identify the trap cause and handle it —
no scheduling decisions.

```c
void user_trap(void) {
    // 1. Switch stvec to kernel_vec.
    //    If another trap fires while we handle this one (e.g., a timer
    //    interrupt during syscall processing), it should go through the
    //    kernel trap path, not the trampoline.
    csrw(stvec, (uint64)kernel_vec);

    // 2. Save the user PC.
    //    sepc holds it now, but yield() could switch to another process
    //    whose trap would overwrite sepc.
    struct proc *p = this_proc();
    p->trapframe->epc = csrr(sepc);

    // 3. Dispatch based on scause.
    uint64 scause_val = csrr(scause);

    if (scause_val == EXC_ECALL_U) {
        // System call — advance PC past the ecall instruction (4 bytes)
        p->trapframe->epc += 4;
        // Enable interrupts during syscall processing (syscalls can be long)
        intr_on();
        // Dispatch to syscall handler (Round 6-2)
        syscall();
    } else if (scause_val & SCAUSE_INTERRUPT) {
        // Timer interrupt — clear SSIP, set flag for scheduling later
        csrw(sip, csrr(sip) & ~SIP_SSIP);
        this_cpu()->need_resched = 1;
    } else {
        // Exception in user mode — mark for death
        kprintf("user_trap: pid=%d %s: scause=%p sepc=%p stval=%p\n",
                p->pid, p->name, scause_val, p->trapframe->epc,
                csrr(stval));
        p->killed = 1;
    }

    // 4. Always exit through user_trap_ret (scheduling + return to user)
    user_trap_ret();
}
```

### Key points about user_trap():

1. **stvec switch**: the first thing it does is point `stvec` back to
   `kernel_vec`. From this point until `user_trap_ret()` restores it,
   any trap goes through the kernel path. This is safe because we're
   on the kernel page table with a kernel stack — exactly the state
   `kernel_vec` expects.

2. **Save sepc immediately**: yield() (in user_trap_ret) → scheduler →
   another process → that process traps → sepc overwritten. Must save
   early.

3. **ecall advances PC by 4**: RISC-V's ecall instruction is always
   4 bytes. The hardware leaves `sepc` pointing AT the ecall (not past
   it). If we returned without advancing, the user would ecall again
   forever.

4. **intr_on() only for syscalls**: syscalls can be long (disk I/O,
   sleep). We re-enable interrupts so the timer can preempt. Timer
   interrupts are brief (just set a flag) — no reason to enable
   nested interrupts for them.

5. **No scheduling logic here**: `user_trap` just dispatches and sets
   flags (`need_resched`, `killed`). The actual yield/exit happens in
   `user_trap_ret` — clean separation of concerns, mirroring how
   `kernel_trap` sets flags and `kernel_trap_ret` acts on them.

### user_trap_ret(): scheduling decisions + return to user

This is the single exit point for all user traps. Whether the trap
was a syscall, a timer, a page fault, or even the first time a new
process enters user mode — everything goes through here.

```c
void user_trap_ret(void) {
    struct proc *p = this_proc();

    // --- Scheduling decisions (mirrors kernel_trap_ret) ---

    // 1. If process was killed, exit now.
    if (p->killed)
        exit(-1);

    // 2. If timer requested preemption, yield.
    if (this_cpu()->need_resched) {
        this_cpu()->need_resched = 0;
        yield();
    }

    // --- Prepare return to user mode ---

    // 3. Disable interrupts while setting up.
    //    We're about to change stvec to the trampoline — if a trap fired
    //    between setting stvec and executing sret, it would go through
    //    user_vec while we're still in the kernel. Bad.
    intr_off();

    // 4. Set stvec to the trampoline's user_vec.
    //    Next trap from user mode will land here.
    csrw(stvec, TRAMPOLINE + (user_vec - trampoline));

    // 5. Prepare trapframe fields for next trap.
    p->trapframe->kernel_satp = csrr(satp);   // current kernel PT
    p->trapframe->kernel_sp = p->kstack + PG_SIZE;  // top of kstack
    p->trapframe->user_trap = (uint64)user_trap;
    p->trapframe->hartid = /* tp */ read_tp();

    // 6. Set up sstatus for sret.
    //    Clear SPP (sret returns to U-mode, not S-mode).
    //    Set SPIE (sret will re-enable interrupts via SIE = SPIE).
    uint64 sstatus_val = csrr(sstatus);
    sstatus_val &= ~SSTATUS_SPP;   // clear SPP → return to U-mode
    sstatus_val |= SSTATUS_SPIE;   // set SPIE → interrupts on after sret
    csrw(sstatus, sstatus_val);

    // 7. Set sepc to the saved user PC (where user execution resumes).
    csrw(sepc, p->trapframe->epc);

    // 8. Compute user page table's SATP value.
    uint64 user_satp = MAKE_SATP(p->pagetable);

    // 9. Jump to user_ret (in the trampoline).
    //    Pass TRAPFRAME VA in a0 (base pointer), user_satp in a1.
    //    user_ret will switch satp, restore regs, and sret.
    uint64 fn = TRAMPOLINE + (user_ret - trampoline);
    ((void (*)(uint64, uint64))fn)(TRAPFRAME, user_satp);
}
```

### Key points about user_trap_ret():

1. **Scheduling first, return second**: the top half mirrors
   `kernel_trap_ret` — check `killed`, check `need_resched`. The bottom
   half prepares the hardware state for returning to user mode.

2. **Single exit point**: every path to user mode goes through here —
   syscalls, interrupts, exceptions, and even the first-ever entry to
   user mode (where `context.ra` points to a stub that calls
   `user_trap_ret` directly).

3. **Interrupts off**: critical window between `csrw(stvec, ...)` and
   `sret`. If an interrupt fired with stvec pointing to the trampoline
   but we're still in S-mode on the kernel PT, chaos follows. RISC-V
   disabling interrupts on trap entry gives us a natural safe window;
   here we explicitly ensure it.

4. **Function pointer trick**: we compute the address of `user_ret`
   within the trampoline page (at its virtual address, not physical).
   Then we call it like a function. This works because the trampoline
   is mapped in the kernel page table too.

5. **Never returns**: `user_ret` ends with `sret`, which jumps to user
   mode. `user_trap_ret()` never returns to its caller. This doesn't
   leak stack frames — `user_ret` loads the user's `sp` from the
   trapframe, abandoning the kernel stack entirely. Next time this
   process traps in, `user_vec` loads `kernel_sp` from the trapframe,
   which always points to the **top** of the kernel stack (set by
   `user_trap_ret`: `p->kstack + PG_SIZE`). The kernel stack starts
   fresh on every trap entry — no accumulation.

---

## Part 8: The Test User Program

### What it does

The simplest possible user program — just proves the boundary works:

```asm
# user/test_user.S — hardcoded user program for Round 6-1
#
# Executes in U-mode. Issues ecall to trap into kernel.
# Kernel prints the argument and kills the process.

.globl _start
_start:
    li  a7, 0       # syscall number (placeholder — no real dispatch yet)
    li  a0, 42      # argument
    ecall           # trap into kernel

    # Should never reach here — kernel handles the ecall
    # But if it does, spin (infinite loop so we don't execute garbage)
spin:
    j   spin
```

### Build process

```makefile
# Makefile additions (simplified)
user/test_user.bin: user/test_user.S
    $(CC) -nostdlib -T user/user.ld -o user/test_user.elf user/test_user.S
    $(OBJCOPY) -O binary user/test_user.elf user/test_user.bin
```

The user linker script (`user/user.ld`) is minimal:

```ld
ENTRY(_start)
SECTIONS {
    . = 0x1000;
    .text : { *(.text .text.*) }
}
```

This ensures the code is linked at VA 0x1000 (matching where we map it).

The binary is embedded into the kernel via `.incbin`:

```asm
# kernel/arch/user_test_bin.S
.section .rodata
.globl test_user_bin
test_user_bin:
    .incbin "user/test_user.bin"
.globl test_user_bin_end
test_user_bin_end:
```

In C:
```c
extern char test_user_bin[];
extern char test_user_bin_end[];
#define test_user_bin_size ((uint64)(test_user_bin_end - test_user_bin))
```

---

## Part 9: bobchouOS vs xv6 & Implementation Plan

### Design comparison

| Aspect | xv6 | bobchouOS | Rationale |
|--------|-----|-----------|-----------|
| User text start VA | 0 | 0x1000 | Null-deref protection from day one |
| Trampoline location | MAXVA − PGSIZE | MAX_VA − PG_SIZE | Same concept, naming conventions differ |
| Trapframe location | MAXVA − 2×PGSIZE | MAX_VA − 2×PG_SIZE | Same concept, naming conventions differ |
| Trampoline file | `trampoline.S` | `trampoline.S` | Same |
| Trap handler names | `uservec`, `userret`, `usertrap`, `usertrapret` | `user_vec`, `user_ret`, `user_trap`, `user_trap_ret` | Snake_case for multi-word names |
| hartid in trapframe | Yes (offset 32) | Yes (offset 24) | Need `tp` for `this_cpu()` |
| Trapframe field order | kernel_satp, kernel_sp, kernel_trap, epc, kernel_hartid | kernel_satp, kernel_sp, user_trap, hartid, epc | Group "things user_vec loads" together; name fields after what they hold |
| User program embedding | Inline initcode[] array in proc.c | Separate .S file + .incbin | Readable source, establishes user/ directory |
| Process creation | Fixed proc array + allocproc | Dynamic kmalloc + alloc_proc | Phase 5 decision |

### Why we reorder hartid before epc

In xv6, `epc` is at offset 24 and `kernel_hartid` is at offset 32.
We put `hartid` at offset 24 instead. Why? The first four
fields (offsets 0–24) are all things that `user_vec` reads immediately
on trap entry. `epc` is written by `user_trap()` AFTER the page table
switch — it doesn't need to be in the "fast path" block. Grouping
the `user_vec` reads together makes the assembly logic clearer:
"load offsets 0, 8, 16, 24 — that's everything I need to enter the
kernel."

### New files

| File | Purpose |
|------|---------|
| `kernel/arch/trampoline.S` | `user_vec` + `user_ret` assembly |
| `kernel/include/trapframe.h` | `struct trapframe` definition |
| `user/test_user.S` | Test user program (3 instructions) |
| `user/user.ld` | User linker script (entry at 0x1000) |
| `kernel/arch/user_test_bin.S` | `.incbin` wrapper for test binary |

### Modified files

| File | Changes |
|------|---------|
| `kernel/include/mem_layout.h` | Add `TRAMPOLINE`, `TRAPFRAME` constants |
| `kernel/include/vm.h` | Declare `proc_pagetable`, `proc_free_pagetable` |
| `kernel/vm.c` | Implement `proc_pagetable`, map trampoline in kernel PT |
| `kernel/trap.c` | Add `user_trap()`, `user_trap_ret()` |
| `kernel/proc.c` | Create test user process, first-entry-to-user glue |
| `kernel/include/proc.h` | Add `user_trap_ret` declaration if needed |
| `linker.ld` | Add `.text.trampoline` section (page-aligned) |
| `Makefile` | Build trampoline, user test binary, embed via incbin |

### Expected output when running

```
bobchouOS is booting...
kernel: 0x80000000 .. 0x8000XXXX (... bytes)
kalloc_init: ...
vm_create_kernel_pt: ...
vm_enable_paging: Sv39 enabled
starting scheduler...
user_trap: ecall from user mode, pid=2, a7=0, a0=42
```

The kernel prints the trap, demonstrating that:
1. User code executed in U-mode (we got an EXC_ECALL_U, not EXC_ECALL_S)
2. Registers were saved correctly (a7=0, a0=42 readable from trapframe)
3. The trampoline worked (we didn't crash switching page tables)
4. The kernel stack and page table are functional (kprintf works)

### What's next

This round (6-1) proves the user/kernel boundary works end-to-end.
The kernel can drop to user mode and recover when the user traps back.
But the `ecall` currently just prints and kills the process — there's
no real syscall dispatch.

**Round 6-2** adds:
- Syscall number dispatch table (a7 → handler function)
- Argument passing (a0–a5 → `argint()`, `argaddr()`)
- First real syscalls: `write()` (print to console) and `exit()`
  (clean process termination from user mode)
- User programs that actually DO something: print "hello world" and
  exit cleanly

---

## Quick Reference

### Constants

| Name | Value | Purpose |
|------|-------|---------|
| `MAX_VA` | 0x80_0000_0000 (2^39) | Maximum Sv39 virtual address |
| `TRAMPOLINE` | MAX_VA − PG_SIZE | VA of trampoline page (both PTs) |
| `TRAPFRAME` | MAX_VA − 2×PG_SIZE | VA of per-proc trapframe (user PT only) |
| `USER_TEXT_START` | 0x1000 | First page of user code |

### Trapframe offsets

| Offset | Field | Size |
|--------|-------|------|
| 0 | kernel_satp | 8 |
| 8 | kernel_sp | 8 |
| 16 | user_trap | 8 |
| 24 | hartid | 8 |
| 32 | epc | 8 |
| 40–280 | user regs x1–x31 in register-number order | 31×8 = 248 |

### Function roles

| Function | Location | Role |
|----------|----------|------|
| `user_vec` | trampoline.S | Save user regs, switch to kernel PT, jump to user_trap |
| `user_ret` | trampoline.S | Switch to user PT, restore user regs, sret |
| `user_trap` | trap.c | Process the trap (syscall/interrupt/exception) |
| `user_trap_ret` | trap.c | Scheduling decisions, prepare CSRs and trapframe, call user_ret |
| `proc_pagetable` | vm.c | Create per-process user page table |

### Page table mappings summary

**Kernel page table:**
```
VA                  → PA                      Perm
────────────────────────────────────────────────────
0x100000            → 0x100000                RW   (QEMU shutdown)
0x2000000           → 0x2000000               RW   (CLINT)
0x0C000000          → 0x0C000000              RW   (PLIC)
0x10000000          → 0x10000000              RW   (UART)
0x80000000..text_end → identity               R-X  (kernel text)
text_end..PHYS_STOP → identity                RW   (kernel data + free mem)
TRAMPOLINE          → PA of trampoline code   R-X  (NEW)
```

**User page table (per process):**
```
VA                  → PA                      Perm
────────────────────────────────────────────────────
TRAMPOLINE          → PA of trampoline code   R-X  (no PTE_U)
TRAPFRAME           → PA of proc's trapframe  RW   (no PTE_U)
0x1000              → PA of user code          R-X + PTE_U
0x3000 (stack)      → PA of stack page         RW  + PTE_U
```

