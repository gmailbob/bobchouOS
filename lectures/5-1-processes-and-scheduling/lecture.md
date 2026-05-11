# Lecture 5-1: Processes, Context Switch & Scheduling

> **Where we are**
>
> Phase 4 is complete. The kernel has a full memory subsystem: buddy
> allocator for physical pages, slab allocator for arbitrary-size kernel
> objects (`kmalloc`/`kmfree`), Sv39 page tables with an identity-mapped
> kernel address space, and generic intrusive containers (`list_head`,
> hash table) ready for use.
>
> But the kernel can only do one thing at a time. `kmain` runs, calls
> test suites, prints results, and shuts down. There is no notion of
> multiple activities sharing the CPU, no way to say "run this function
> and also that function, switching between them." The machine is a
> single-threaded batch processor.
>
> Phase 5 introduces **processes** — the fundamental abstraction that
> turns one CPU into the illusion of many. Each process has its own
> register state, its own stack, and (eventually) its own address space.
> The kernel multiplexes the single hart between these processes,
> switching rapidly enough that all appear to run simultaneously.
>
> This first lecture builds the scheduler and context switch mechanism.
> By the end, bobchouOS will run multiple kernel threads that
> interleave on the single hart — proving the mechanism works before we
> tackle user-mode processes in Phase 6.
>
> **What you will understand after this lecture:**
>
> - What a process is (state + execution context)
> - `struct proc` — fields, lifecycle states, multiple list memberships
> - `struct context` — why only callee-saved registers
> - `struct cpu` — per-hart scheduler state, `this_cpu()`, `this_proc()`
> - `swtch()` — the assembly routine that saves one context and restores
>   another
> - The scheduler loop — how it picks the next process and switches to it
> - Kernel threads — processes that run in S-mode as a first milestone
> - `yield()` — voluntary context switch, then timer-driven preemption
>
> **xv6 book coverage:** This lecture absorbs Chapter 7 (Scheduling),
> sections 7.1 (Multiplexing), 7.2 (Code: Context switching), 7.3
> (Code: Scheduling), 7.4 (Code: mycpu and myproc). Sections on sleep/
> wakeup (7.5–7.10) and locking are deferred to later rounds.

---

## Part 1: What Is a Process?

### The illusion

A modern OS runs hundreds of processes on a handful of CPUs. Each
process *believes* it has its own dedicated processor — its own
registers, its own program counter, its own stack. This is an illusion
maintained by the kernel: it rapidly switches the CPU between processes,
saving and restoring their state each time.

The key insight: **a process is not a program.** A program is static
code on disk. A process is a *running instance* of a program — it has
dynamic state that changes on every instruction:

| Static (the program) | Dynamic (the process) |
|----------------------|-----------------------|
| Text (instructions) | Program counter (which instruction is next) |
| Initialized data | Register values (computation in progress) |
| | Stack (call frames, local variables) |
| | Heap allocations |
| | Kernel metadata (PID, state, open files) |

Two processes can run the same program (e.g., two shell instances) but
have completely different dynamic state.

### What the kernel must track per process

For each process, the kernel maintains:

1. **Identity** — PID, name (for debugging)
2. **Execution state** — saved registers (the *context*) when the
   process is not running
3. **Scheduling state** — is it runnable? running? sleeping? dead?
4. **Memory** — kernel stack, (later) user page table and user memory
5. **Relationships** — parent, children
6. **Resources** — (later) open files, current directory

All of this lives in `struct proc` — one per process, allocated via
`kmalloc`.

### Process lifecycle states

For Round 5-1, we only need two states — a simple loop:

```
                 +--------yield / preempt----------+
                 |                                 |
                 v                                 |
creation --> [RUNNABLE] ---scheduler picks---> [RUNNING]
```

SLEEPING, ZOMBIE, and the full lifecycle state machine (exit, wait,
sleep/wakeup) arrive in Round 5-2.

---

## Part 2: `struct proc` and Supporting Structures

### `struct proc`

We allocate `struct proc` dynamically via `kmalloc` — no fixed-size
array like xv6's `proc[NPROC]`. Each proc participates in multiple
data structures simultaneously via embedded `list_head` nodes (as we
learned in Lecture 5-0):

```c
enum proc_state {
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_ZOMBIE,
};

struct proc {
    /* --- Identity --- */
    int pid;
    char name[16];
    enum proc_state state;

    /* --- Scheduling (embedded list nodes) --- */
    struct list_head all_list;     // global all-processes list
    struct list_head run_list;     // run queue (only when RUNNABLE)
    struct list_head pid_link;     // PID hash table bucket chain

    /* --- Family (Round 5-2) --- */
    struct proc *parent;
    struct list_head children;    // head of my children list
    struct list_head sibling;     // link in parent's children list

    /* --- Execution state --- */
    struct context context;       // saved callee-regs for swtch
    uint64 kstack;                // kernel stack base address

    /* --- Address space (Phase 6) --- */
    pagetable_t pagetable;        // user page table
    struct trapframe *trapframe;  // saved user regs for trap
    uint64 sz;                    // user memory size

    /* --- Exit (Round 5-2) --- */
    int exit_status;
};
```

Fields for later rounds are declared now but unused — this avoids
changing struct layout (and kmalloc size class) when those features
arrive. Linux's `task_struct` works the same way: one giant struct,
fields activated as subsystems come online.

### `struct context` — what gets saved on a switch

When a process is not running, its state must be saved somewhere. But
we don't need all 32 registers.

`swtch()` is called as a **normal C function** from kernel code —
either by the scheduler (switching into a process) or by `yield()`
(switching back to the scheduler). The compiler obeys the RISC-V
calling convention at the call site:

- **Caller-saved registers** (a0–a7, t0–t6) are already saved on the
  stack by the calling code — that's what "caller-saved" means.
- **Callee-saved registers** (sp, s0–s11) are `swtch`'s responsibility.

So `swtch` saves 14 registers: **sp, s0–s11** (callee-saved) plus
**ra** (the resume address).

> **Wait — isn't `ra` caller-saved?** Technically yes. But `swtch` saves
> `ra` for a different reason: it's the **address to resume from** when
> this context is restored later. For a running process that called
> `yield()` → `swtch()`, `ra` holds the return address back into
> `yield`. For a brand-new kernel thread, we manually set `ra` to the
> thread's entry function. Either way, `ra` is the "resume point."
> Our `kernel_vec.S` also saves `ra`, but for the standard caller-saved
> reason: the `call kernel_trap` instruction would overwrite it.

```c
struct context {
    uint64 ra;    // resume address (see note above)
    uint64 sp;    // stack pointer
    uint64 s0;    // callee-saved registers
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};
```

### Two stacks per process

Every process has **two stacks**:

- **Kernel stack** — used whenever the process executes in kernel mode
  (trap handlers, syscalls, `yield()`, `swtch`). One page, allocated at
  process creation, always valid.
- **User stack** — (Phase 6) used when running in U-mode. Saved
  separately in the trapframe when entering the kernel.

Context switch always happens in kernel mode (inside `yield()` →
`swtch()`), so `context.sp` always points into the kernel stack.

> **Can a user process have an empty kernel stack?** In theory, yes — if
> it never traps. In practice, impossible: timer interrupts fire every
> ~10ms regardless, I/O requires syscalls, and program exit requires
> `exit()`. The kernel stack is allocated "just in case" because when a
> trap fires, you need a valid stack *immediately* — you can't allocate
> one at trap time (you'd need a stack to call kmalloc).

In Round 5-1, our kernel threads have only a kernel stack (no user
stack). The two-stack model becomes visible in Phase 6.

### `struct cpu` — per-hart state

Even with one hart, we need a place to store the scheduler's own
context and a pointer to the currently running process:

```c
struct cpu {
    struct proc *proc;            // currently running process, or NULL
    struct context scheduler;     // scheduler's saved context
    int need_resched;             // timer set this; ret_from_trap checks
};
```

Accessors:

```c
struct cpu *this_cpu(void);    // returns current hart's cpu struct
struct proc *this_proc(void);  // returns this_cpu()->proc
```

With one hart, `this_cpu()` just returns `&cpus[0]`. On multi-hart
systems, each hart stores its ID in the `tp` register at boot;
`this_cpu()` reads `tp` and indexes into `cpus[]`.

> **The `tp` register (x4):** RISC-V reserves `tp` as the "thread
> pointer" — no function is allowed to modify it (not caller-saved, not
> callee-saved, just permanently off-limits). In userspace, `tp` points
> to thread-local storage (TLS). In kernel code, we repurpose it as a
> **hart identifier**: each hart sets `tp` once at boot (e.g., to its
> hart ID or directly to `&cpus[hartid]`), and it never changes after
> that. This makes `this_cpu()` a single register read — no memory
> access, no lock, works even with interrupts disabled.

We introduce this indirection now so that adding a second hart later
doesn't require rewriting every function that references "the current
process."

### PID allocation

Simple monotonic counter starting at 1. PID 0 is reserved for the idle
thread — `proc_create_kernel` for idle explicitly sets `p->pid = 0`
instead of calling `alloc_pid()`:

```c
static int next_pid = 1;

static int
alloc_pid(void) {
    return next_pid++;
}
```

No reuse, no bitmap. With a 32-bit counter, we'd need to create 2
billion processes before wrapping — not a concern for a toy OS. Linux
uses an IDR (radix tree) for PID allocation with reuse; we skip that
complexity.

> **Why start at 1?** Unix convention: PID 0 is reserved for the
> scheduler/idle task, PID 1 is `init` (first real process). Starting at
> 1 also means PID 0 can serve as a "no process" sentinel, and
> `if (pid)` works as a validity check.
>
> **What is PID 1 in practice?** On Linux, the kernel creates a kernel
> thread (`kernel_init`) that eventually calls `execve("/sbin/init")` —
> that becomes PID 1 in user mode. On most modern distros, PID 1 is
> `systemd`; on older systems, SysVinit's `/sbin/init`. In Docker
> containers, PID 1 is whatever the `ENTRYPOINT` specifies (`bash`,
> `nginx`, your app). PID 1 is special: the kernel sends orphaned
> processes to it, and when PID 1 exits, the system (or container)
> shuts down — this is how container orchestrators like AWS ECS detect
> task completion (PID 1 exited → container is done).
>
> For bobchouOS: PID 0 is the idle thread (runs `wfi` when nothing else
> is runnable), PID 1+ are worker kernel threads in Round 5-1. Later
> (Phase 6), PID 1 becomes the first user process.

---

## Part 3: `swtch` — The Context Switch

### What it does

`swtch` is a short assembly routine that:
1. Saves the current context (14 registers) to `*old`
2. Restores a previously saved context from `*new`
3. Executes `ret` — which jumps to `new->ra`, NOT back to swtch's
   caller. The whole point is to land somewhere else:
   - Switching to a process → `new->ra` points inside that process's
     `yield()`, so execution resumes in the process
   - Switching to the scheduler → `new->ra` points inside
     `scheduler()`, so execution resumes in the scheduler loop
   - Switching to a brand-new thread → `new->ra` points to the
     thread's entry function, so execution starts there

```
void swtch(struct context *old, struct context *new);
```

After `swtch` returns, the CPU is running on a *different stack*, with
*different* callee-saved registers, continuing from wherever the `new`
context was last saved. From the new context's perspective, its earlier
call to `swtch` just returned.

### The assembly

```asm
# swtch(struct context *old, struct context *new)
# a0 = old, a1 = new
swtch:
    # Save current callee-saved registers into old context
    sd ra,  0(a0)
    sd sp,  8(a0)
    sd s0, 16(a0)
    sd s1, 24(a0)
    sd s2, 32(a0)
    sd s3, 40(a0)
    sd s4, 48(a0)
    sd s5, 56(a0)
    sd s6, 64(a0)
    sd s7, 72(a0)
    sd s8, 80(a0)
    sd s9, 88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    # Restore callee-saved registers from new context
    ld ra,  0(a1)
    ld sp,  8(a1)
    ld s0, 16(a1)
    ld s1, 24(a1)
    ld s2, 32(a1)
    ld s3, 40(a1)
    ld s4, 48(a1)
    ld s5, 56(a1)
    ld s6, 64(a1)
    ld s7, 72(a1)
    ld s8, 80(a1)
    ld s9, 88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)

    ret    # jump to new ra
```

That's it — 29 instructions, no magic. The "magic" is that after `ld sp`
we're on a different stack, and after `ret` we're in a different
function. The CPU has no idea a "context switch" happened; it just
loaded some registers and jumped.

> **Why assembly, not C?** A C function's prologue/epilogue always saves
> and restores `ra` to return to *its caller*. But `swtch` needs to load
> a *different* `ra` (from the new context) and `ret` to that instead.
> The compiler has no way to express "load ra from this struct, then
> ret" — it would always restore the original ra from its own prologue.
> Writing in assembly gives us full control over which `ra` is in the
> register when `ret` executes.

### Tracing a full context switch

Let's follow exactly what happens when process A calls `yield()`:

```
A is running:

function_A:
    ...
    call yield              # ra = return-to-A address; jump to yield

yield:
    sd ra, 0(sp)            # yield's prologue: save ra (= return-to-A) on stack
    ...                     # p->state = RUNNABLE; list_add_tail(...)
    call swtch              # ra = instruction-after-this-call (inside yield)
                            # jump to swtch

swtch(old=&A.context, new=&cpu.scheduler):
    sd ra,  0(a0)           # A.context.ra = "inside yield, after call swtch"
    sd sp,  8(a0)           # A.context.sp = A's kernel stack pointer
    sd s0, 16(a0)           # ... save all callee-saved
    ...
    ld ra,  0(a1)           # load scheduler's ra
    ld sp,  8(a1)           # *** STACK SWITCH: now on boot stack ***
    ld s0, 16(a1)           # ... restore scheduler's callee-saved
    ...
    ret                     # jump to scheduler's ra → back inside scheduler()
```

Later, when the scheduler picks A again:

```
scheduler:
    swtch(&cpu.scheduler, &A.context)

swtch(old=&cpu.scheduler, new=&A.context):
    sd ra,  0(a0)           # save scheduler's ra
    sd sp,  8(a0)           # save scheduler's sp (boot stack)
    ...
    ld ra,  0(a1)           # load A.context.ra = "inside yield, after call swtch"
    ld sp,  8(a1)           # *** STACK SWITCH: now on A's kernel stack ***
    ...
    ret                     # jump to ra → back inside yield()

yield (resumed):
    ...                     # epilogue
    ld ra, 0(sp)            # restore ra = return-to-A address
    ret                     # jump back into function_A
```

The two-level return:
1. `context.ra` gets you back into `yield()` (where `swtch` was called)
2. `yield()`'s stack frame gets you back into `function_A` (the original
   caller)

### No stack overlap

Each participant has its own stack at a different physical address:

```
Process A's kstack:    0x80050000 - 0x80051000  (kalloc'd page)
Process B's kstack:    0x80052000 - 0x80053000  (kalloc'd page)
Scheduler's stack:     boot stack from entry.S  (separate memory)
```

When `swtch` does `ld sp, 8(new)`, it loads a completely different
address — there is no overlap. That's why each process gets its own
`kalloc()`-ed page for its kernel stack.

### How `swtch` is NOT like a trap

| | Trap (kernel_vec) | swtch |
|--|--|--|
| Trigger | Hardware interrupt or exception | Explicit function call |
| Registers saved | All 32 | 14 (callee-saved + ra) |
| Saved where | Kernel stack (trapframe) | `struct context` |
| Returns to | Same point that was interrupted | Where `new->ra` points |
| Voluntary? | No — can happen anywhere | Yes — only from known call sites |

Traps save all registers because they can fire at any point — the
hardware can't know which registers are "important." `swtch` is a
voluntary function call — the calling convention guarantees caller-saved
registers are already on the stack, so `swtch` only handles the callee
half. This makes context switch much cheaper than a full trap frame
save/restore.

---

## Part 4: The Scheduler

### The scheduler is not a process

The scheduler has no `struct proc`, no PID, no entry in `all_procs`. It
is the kernel's **main loop** — the final thing `kmain()` does:

```
kmain()
  → kalloc_init()         physical page allocator
  → vm_create_kernel_pt() kernel page table
  → vm_enable_paging()    enable Sv39
  → kmalloc_init()        slab allocator
  → proc_init()           run queue, PID table, cpu struct
  → proc_create_kernel()  create kernel threads
  → scheduler()           start scheduling (never returns)
```

At this point, `kmain()`'s stack (the boot stack from `entry.S`)
*becomes* the scheduler's permanent stack. The scheduler's context is
stored in `struct cpu`, not in a `struct proc`. PID 0 (idle thread) is
the first kernel thread created; PID 1+ are worker threads.

### The two-hop design

A process never switches directly to another process — it always goes
through the scheduler:

```
Process A          Scheduler          Process B
    |                  |                  |
    |--- swtch ------->|                  |
    |                  |--- swtch ------->|
    |                  |                  |
    |                  |<--- swtch -------|
    |<--- swtch -------|                  |
    |                  |                  |
```

This means the scheduler always gets to run its selection logic between
processes. No process can "hijack" the switch and jump directly to
another.

### The scheduler loop

```c
void
scheduler(void) {
    struct cpu *c = this_cpu();

    for (;;) {
        // With the idle thread always in the queue, this never returns NULL.
        struct proc *p = pick_next();  // pop from run queue head

        p->state = PROC_RUNNING;
        c->proc = p;

        // Switch from scheduler context to process context
        swtch(&c->scheduler, &p->context);

        // We return here when the process switches back to us
        c->proc = NULL;
    }
}
```

### `yield()` — giving up the CPU

When a process wants to stop running (voluntarily or forced by timer):

```c
void
yield(void) {
    struct proc *p = this_proc();
    p->state = PROC_RUNNABLE;

    // Push back onto run queue tail (FIFO: round-robin)
    list_add_tail(&p->run_list, &run_queue);

    // Switch back to scheduler
    swtch(&p->context, &this_cpu()->scheduler);
}
```

After `yield()`, the process's context is saved and the scheduler
resumes. When it's this process's turn again, the scheduler's `swtch`
returns — which means *this* process's `yield()` returns, and execution
continues from where it left off.

### Round-robin policy

The run queue is a FIFO list:
- `yield()` pushes the process to the **tail**
- `pick_next()` pops from the **head**

This gives every process a fair turn. It's the simplest possible policy
— and it's what xv6 uses (effectively, since it scans the array in
order).

### How real OSes schedule

| OS | Policy | Data structure | How `pick_next` works |
|----|--------|---------------|----------------------|
| bobchouOS | Round-robin | FIFO list | Pop head |
| xv6 | Round-robin | Array scan | First RUNNABLE in array |
| Linux (CFS) | Completely Fair | Red-black tree keyed by `vruntime` | Leftmost node (lowest runtime) |
| Windows | Multi-level priority | 32 priority queues | Pop from highest non-empty queue |
| FreeBSD (ULE) | Priority + interactivity | Per-CPU run queues with decay | Highest priority, with boost for interactive tasks |

The key insight: **the scheduling policy is decoupled from the context
switch mechanism.** All these OSes use essentially the same `swtch`-style
save/restore underneath. Only `pick_next()` and the insertion logic
differ. Swapping bobchouOS to priority scheduling later would change
a few lines in the scheduler — not `swtch`, not `yield`, not
`struct context`.

### Timer preemption

We already have timer interrupts from Phase 2 (CLINT timer — currently
periodic, but we'll switch to on-demand one-shot timers below). To make
scheduling preemptive, we call `yield()` from the timer handler:

```c
// In kernel_trap(), timer interrupt case:
if (this_cpu()->proc != NULL)
    yield();
```

We check `proc != NULL` because the timer can fire while the
**scheduler itself** is running (between processes, spinning in the
`for(;;)` loop). In that case there's no process to preempt — just
return from the trap and let the scheduler continue.

The full preemption flow:

```
Process A executing user/kernel code
  → timer interrupt fires (hardware)
  → CPU jumps to kernel_vec.S (trap vector)
  → kernel_vec saves all 32 regs on A's kernel stack
  → calls kernel_trap()
  → kernel_trap() detects timer, calls yield()
  → yield() calls swtch() → scheduler resumes
```

Note that `context.ra` still points inside `yield()` — even for a
timer-preempted process. The timer doesn't bypass `yield()`; it *calls*
it. When the process is scheduled again, it returns from `yield()` →
`kernel_trap()` → `kernel_vec` restores all 32 regs → `sret` back to
the exact instruction that was interrupted. The process never knows it
was preempted.

### The timeslice problem

Our current M-mode timer handler does `mtimecmp += interval` — it
schedules the next tick relative to the *last* tick. This creates an
unfairness: a process picked up by the scheduler at t=19.9ms will be
preempted at t=20.0ms — only 0.1ms of actual runtime, while the
previous process got nearly 10ms.

```
Timeline (10ms ticks):
    t=10        t=20        t=30
     |           |           |
     |  A runs   | B runs    |
     |  9.9ms    | 0.1ms !   |
     |           |           |
     scheduler   scheduler
     picks A     picks B
     at t=10.1   at t=19.9
```

The fix: **reset the timer when the scheduler picks a process.** Instead
of a fixed global tick, set `mtimecmp = mtime + interval` each time we
switch to a process. This guarantees every process gets its full
timeslice regardless of when it was scheduled. Linux uses the same
approach — it programs a one-shot timer (APIC on x86, `mtimecmp` on
RISC-V) at each context switch rather than relying on a fixed periodic
tick. (Linux calls this "tickless" or `NO_HZ_FULL` mode.)

### How S-mode talks to the timer hardware

The scheduler needs to do two things:
1. **Read** `mtime` — to know the current time
2. **Write** `mtimecmp` — to arm the next timeslice deadline

Both are CLINT registers, memory-mapped at physical addresses (not
CSRs). The question is: can S-mode do these directly, or must it ask
M-mode?

### Access control: PMP and page tables

As covered in Lecture 2-3, MMIO access is controlled by **PMP** (not
privilege level). We configured PMP to allow all non-M-mode code full
access to the physical address space. So PMP alone would let S-mode
(and even U-mode) access CLINT directly.

But with paging enabled (since Round 4-1), S-mode also needs a **page
table mapping**. Before Round 5-1, S-mode never accessed CLINT — the
M-mode timer handler did all CLINT MMIO, and S-mode only touched CSRs
(which bypass paging). Now that the scheduler calls `read_mtime()`,
that load goes through the page table. Without a mapping for
`0x2000000`, it page faults.

So we add CLINT to the kernel page table:
```c
kvm_map(CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);
```
64KB covers the full CLINT region (msip + mtimecmp + mtime).

The full access control stack:

```
M-mode   → PMP doesn't apply (always allowed), satp ignored
S-mode   → PMP check (allow all) → page table (CLINT mapped, U=0 → S can access)
U-mode   → PMP check (allow all) → page table (CLINT not in user PT → fault)
```

PMP doesn't distinguish S from U — it only separates M-mode from
everything below. So what stops U-mode? The page table: the kernel
won't map CLINT into user page tables, so a U-mode access faults
before the physical address ever hits the bus (Lecture 4-1, PTE `U`
bit).

### What S-mode does directly vs via ecall

On our QEMU, S-mode *could* do both reads and writes to CLINT directly
(PMP and page table allow it). But on real hardware, firmware
configures PMP to restrict dangerous writes:

| Access | Real firmware allows S-mode? | Why |
|--------|------------------------------|-----|
| Read `mtime` | Yes | Harmless counter; blocking forces every time query through ecall |
| Write `mtimecmp` | No — SBI ecall only | Controls when interrupts fire; buggy kernel could hang the system |
| Write shutdown device | No — SBI ecall only | Could halt the machine unexpectedly |
| Read `mtimecmp` | Usually yes | Just seeing the current deadline, harmless |

We follow this model for portability and learning:
- **`read_mtime()`** — direct MMIO read from S-mode (one `ld`
  instruction, no ecall overhead)
- **`sbi_set_timer(deadline)`** — ecall to M-mode, which writes
  `mtimecmp`
- **`sbi_shutdown()`** — ecall to M-mode, which writes the QEMU
  shutdown device

### Our mini-SBI

On real RISC-V systems, firmware (OpenSBI) provides SBI services. We
run with `-bios none` — there is no OpenSBI. Our own `entry.S` *is*
the M-mode code. So we write our own **mini-SBI**: S-mode issues
`ecall`, which traps into M-mode (`mcause = 9`). Our handler dispatches
on `a7`:

```asm
// S-mode wrapper (sbi.S) — normal kernel function, linked in kernel .text:
sbi_set_timer:          // a0 = deadline (caller put it there per convention)
    li  a7, SBI_SET_TIMER
    ecall
    ret
```

```
S-mode (kernel .text, kernel stack):           M-mode (m_vec.S, no stack):
+------------------------------------+         +------------------------------+
| scheduler()                        |         |                              |
|   -> sbi_set_timer(deadline)       |         |                              |
|       li  a7, SBI_SET_TIMER        |         |                              |
|       ecall --------------------------->     | m_vec:                       |
|                                    |         |   csrr mcause -> ecall from S|
|       (blocked until mret)         |         |   read a7 -> SBI_SET_TIMER   |
|                                    |         |   write a0 to mtimecmp       |
|       ret  <-----------------------------    |   mepc += 4                  |
|   (continues in scheduler)         |         |   mret --------------------> |
+------------------------------------+         +------------------------------+
```

M-mode writes `deadline` to `mtimecmp`, advances `mepc` past the
`ecall` instruction (+4 bytes), and `mret` back to S-mode.

> **How OpenSBI compares:** OpenSBI is primarily C with a thin assembly
> entry. It has its own M-mode stack, own memory region (PMP-protected
> from S-mode), and a full C runtime — essentially a small firmware OS.
> Our handler is ~20 instructions total; writing it in C would require
> setting up a separate M-mode stack and call frame — more overhead than
> the actual work.

> **Alternative S-mode wrapper in C (what Linux does):**
>
> ```c
> void sbi_set_timer(uint64 deadline) {
>     register uint64 a0 asm("a0") = deadline;
>     register uint64 a7 asm("a7") = SBI_SET_TIMER;
>     asm volatile("ecall" : : "r"(a0), "r"(a7));
> }
> ```
>
> `register ... asm("a0")` is a GCC extension that pins a C variable to
> a specific hardware register. We use the assembly version instead —
> it's three instructions doing three instructions of work, no syntactic
> overhead.

### Preview: same pattern at the next privilege boundary

This ecall mechanism is identical to user→kernel syscalls (Phase 6),
just one privilege level lower:

| | Round 5-1 (S → M) | Phase 6 (U → S) |
|--|--|--|
| Instruction | `ecall` from S-mode | `ecall` from U-mode |
| Traps to | M-mode (`m_vec.S`) | S-mode (`kernel_vec.S`) |
| mcause / scause | 9 (env call from S) | 8 (env call from U) |
| Dispatch on | `a7` (function ID) | `a7` (syscall number) |
| Returns via | `mret` | `sret` |
| First use case | `set_timer`, `shutdown` | `exit`, `write`, `fork` |

By the time we build user syscalls, you'll have already written the
pattern once.

> **Dispatch styles:** Our M-mode handler uses cascading `beq`
> comparisons (check a7 against each function ID). This is fine for 2
> functions. With more cases, you'd use a **jump table** — an array of
> addresses indexed by function number:
>
> ```asm
> la    t0, sbi_table
> slli  t1, a7, 3         # index × 8 (each entry is an address)
> add   t0, t0, t1
> ld    t0, 0(t0)         # load handler address
> jr    t0                # jump
>
> sbi_table:
>     .quad m_set_timer   # a7=0
>     .quad m_shutdown    # a7=1
> ```
>
> For Phase 6 user syscalls (dozens of handlers), we'll use a C function
> pointer array instead — same concept (indexed dispatch), but in C
> where it's easier to maintain:
> `static int (*syscalls[])(void) = { [SYS_exit] = sys_exit, ... };`

### The scheduler with timer

```c
void
scheduler(void) {
    struct cpu *c = this_cpu();
    for (;;) {
        struct proc *p = pick_next();

        p->state = PROC_RUNNING;
        c->proc = p;
        sbi_set_timer(read_mtime() + TIMER_INTERVAL);  // arm one-shot

        swtch(&c->scheduler, &p->context);
        c->proc = NULL;
    }
}
```

`read_mtime()` is a direct MMIO read (fast, no ecall). `sbi_set_timer`
goes through ecall to M-mode (which writes `mtimecmp`). Each process is
guaranteed `TIMER_INTERVAL` of runtime from the moment it starts.

### Why not handle scheduling entirely in M-mode?

Since M-mode receives the timer interrupt directly, you might ask: why
forward to S-mode at all? Why not have M-mode call `swtch` and do the
context switch itself?

It would work mechanically on bare metal — but creates severe problems:

| Concern | Why M-mode scheduling is problematic |
|---------|--------------------------------------|
| Privilege | M-mode is unrestricted — a scheduler bug could corrupt firmware state or brick the machine |
| Complexity | Scheduler needs `struct proc`, run queue, list operations — half the kernel moves into M-mode |
| Paging | **M-mode never uses page tables** — the RISC-V spec guarantees M-mode addresses are physical, `satp` is ignored. All kernel data structures use virtual addresses once paging is on; M-mode can't dereference them |
| Reentrancy | If a process was mid-ecall when the timer fires, M-mode is now handling two things at once |
| Real-world | No OS does this — M-mode is firmware, not kernel |

> **M-mode and paging:** This is a hardware guarantee, not a software
> choice. The RISC-V spec states that `satp` only affects S-mode and
> U-mode translation. M-mode loads/stores always go directly to the
> physical address bus, regardless of `satp`. This is why M-mode can
> always reach device registers and DRAM even if the kernel's page tables
> are broken — it's the recovery layer.

The right split:

```
M-mode (tiny, trusted — our mini-SBI):
  - Boot setup (runs once)
  - ecall dispatch:
      a7 = SBI_SET_TIMER  → write a0 to mtimecmp
      a7 = SBI_SHUTDOWN   → write to QEMU shutdown device
  - Timer interrupt → set SSIP (forward to S-mode)

S-mode (the kernel):
  - Everything else: scheduling, processes, memory, drivers
```

We'll also move the `QEMU_SHUTDOWN` write behind the SBI interface —
S-mode currently pokes the shutdown device directly, but it belongs in
M-mode for the same reason as the timer: S-mode shouldn't need device
addresses, and the clean privilege chain (user → syscall → S-mode →
ecall → M-mode) works for shutdown too.

Principle: **least privilege, smallest trusted base.** M-mode does the
minimum that requires machine privilege, then gets out of the way.

### On-demand timer (no global tick)

Since we now reset `mtimecmp` via ecall each time the scheduler picks a
process, we no longer need a periodic global tick. We can disable the
old `mtimecmp += interval` logic entirely — the timer only fires once
per timeslice, exactly when needed. No wasted ticks while the scheduler
is idle spinning, and no timer interrupts firing during init code before
any process exists.

For Round 5-1, we start by testing with explicit `yield()` calls, then
enable timer preemption + per-process timeslice once the basic mechanism
is proven correct.

---

## Part 5: Kernel Threads — First Milestone

### What they are

A kernel thread is a process that runs entirely in S-mode. It shares
the kernel page table with all other kernel threads (no per-process user
address space). It has its own kernel stack and context, so it can be
scheduled independently.

This is our first milestone because it tests the full scheduling
mechanism without introducing user page tables or the trampoline
(Phase 6 concerns). Kernel threads aren't just for testing — real
kernels use them permanently for background tasks (Linux's `kswapd`
for page reclaim, `ksoftirqd` for deferred interrupt work, `kworker`
for async work queues).

### Creating a kernel thread

```c
struct proc *
proc_create_kernel(void (*fn)(void), const char *name) {
    struct proc *p = kmalloc(sizeof(struct proc));
    // ... initialize fields, allocate kstack ...

    // Set up context so that swtch "returns" into fn
    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)fn;       // "return" to this function
    p->context.sp = p->kstack + PG_SIZE;  // top of kernel stack

    p->state = PROC_RUNNABLE;
    list_add_tail(&p->run_list, &run_queue);
    hash_add(pid_table, &p->pid_link, PID_HASH_BITS, hash_int(p->pid));
    return p;
}
```

The trick: we set `context.ra` to the thread's entry function. When
the scheduler does `swtch(&c->scheduler, &p->context)`, it loads these
registers — including `ra = fn` — and `ret` jumps to `fn`. The thread
starts executing as if it were called from nowhere. When it wants to
give up the CPU, it calls `yield()`.

### The idle thread (PID 0)

Our first kernel thread is the **idle thread** — it runs when nothing
else is runnable:

```c
void
idle_thread(void) {
    for (;;) {
        wfi();     // halt hart until next interrupt (zero power)
        yield();   // interrupt woke us — let scheduler check for real work
    }
}
```

`wfi` (Wait For Interrupt) is a RISC-V instruction that halts the hart
in a low-power state until an interrupt arrives. On QEMU it doesn't
matter (virtual CPU), but on real hardware this is essential for power
management — idle cores don't burn energy.

The idle thread gets PID 0 (matching Linux's convention: PID 0 is the
idle/swapper task on the boot CPU). It's always in the run queue as the
lowest-priority fallback. The scheduler never has an "empty queue" case
— when no real work exists, idle wins and the hart sleeps.

| Approach | xv6 (spin in scheduler) | bobchouOS (idle thread) |
|----------|-------------------------|-------------------------|
| When queue empty | `continue` loop — CPU burns cycles | `wfi` — CPU halts |
| Special case in scheduler | Yes (`if (!p) continue`) | No — idle is just another process |
| Power on real hardware | Wastes energy | Near-zero |

### What the test looks like

```c
void
worker(void) {
    int count = 0;
    for (;;) {
        count++;
        if (count % 1000000 == 0)
            kprintf("[%s] %d\n", this_proc()->name, count / 1000000);
        if (count % 3000000 == 0)
            yield();  // simulate blocking I/O every 3M iterations
        if (count >= 10000000)
            count = 0;
    }
}

// In main:
proc_create_kernel(idle_thread, "idle");    // PID 0
proc_create_kernel(worker, "worker_a");     // PID 1
proc_create_kernel(worker, "worker_b");     // PID 2
scheduler();  // never returns
```

This demonstrates three things:
- **Timer preemption** — between voluntary yields, the worker runs
  millions of iterations; the timer forces switches every 10ms
  regardless of what the process is doing
- **Voluntary yield** — every 3M iterations, the worker "blocks"
  (simulates I/O wait), giving up its remaining timeslice early
- **Interleaving** — output shows both workers making progress
  concurrently on a single hart

Expected output (non-deterministic — depends on how many iterations
fit in 10ms):
```
[worker_a] 1
[worker_a] 2
[worker_b] 1     ← timer preempted a, scheduler picked b
[worker_a] 3
[worker_b] 2
[worker_b] 3     ← a voluntarily yielded at 3M, b got extra time
...
```

The 3M yield interval is deliberately not aligned with 1M print
interval — you'll see some prints clustered and some interrupted
mid-sequence, making preemption visible in the output.

This proves: `swtch` works, scheduler picks from run queue, timer
preemption forces switches, voluntary yield works, multiple processes
coexist, PID table is populated, idle thread runs when others yield.

---

## Part 6: bobchouOS vs xv6

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Process allocation | Fixed `proc[64]` array | Dynamic `kmalloc` |
| Run queue | Implicit (scan all procs) | Explicit `list_head` FIFO |
| PID lookup | O(n) array scan | O(1) hash table |
| Scheduler context | Per-CPU `struct context` | Same (`struct cpu`) |
| `swtch` | 14 callee-saved regs | Same |
| Naming | `mycpu()` / `myproc()` | `this_cpu()` / `this_proc()` |
| Timer | Fixed periodic tick (~100 Hz) | On-demand one-shot per-process timeslice via SBI ecall |
| First milestone | Directly creates user proc | Kernel threads first |
| Locking | `p->lock` per process | Deferred (single hart) |

---

## Part 7: Debugging the Timer Preemption Bug

This section documents the bugs encountered when implementing timer-driven
preemption and the reasoning behind each fix. The issues are subtle
consequences of calling `yield()` inside a trap handler — a design choice
we share with xv6, but which creates pitfalls that aren't obvious from the
lecture's high-level description.

### Background: process switch inside a trap

To understand the bugs, we must first trace exactly what happens when a
process is preempted by the timer. The key insight: `sret` DOES run —
just not immediately. The trap frame is frozen mid-execution while other
processes run.

**Full trace of timer preemption:**

```
Process A running
  → timer fires
  → CPU jumps to kernel_vec.S (stvec)
  → kernel_vec saves all regs on A's kernel stack
  → call kernel_trap()
    → kernel_trap detects IRQ_S_SOFT
    → clears SSIP
    → calls yield()
      → yield calls swtch(&A.context, &cpu.scheduler)
      → *** we leave A's execution here ***

... scheduler runs, picks B, switches to B ...
... B runs its timeslice, yields back to scheduler ...
... eventually scheduler picks A again ...

      → swtch returns (back inside yield)
    → yield returns (back inside kernel_trap)
  → kernel_trap returns (back inside kernel_vec)
  → kernel_vec restores all regs
  → sret ← this runs when A is finally resumed
```

The `sret` executes — just delayed by however long A was switched out.
When `yield()` calls `swtch`, we leave A's kernel stack "mid-function."
The entire call stack is frozen:

```
A's kernel stack (frozen while A is not running):
  kernel_vec frame (saved all regs)
    kernel_trap frame
      yield frame
        ← sp is here when swtch saves it into A.context
```

When A is scheduled again, `swtch` restores this sp, `yield` returns,
`kernel_trap` returns, `kernel_vec` restores all regs, `sret` jumps
back to the exact instruction A was running when the timer fired.

This "suspended trap" design is what creates the bugs below.

### Background: stack frames and the frame pointer

To understand Bug 2, we need to know how function calls use the stack.
(Lecture 0-2, Part 5 showed prologue/epilogue code but didn't name
the concept explicitly. Here we define it.)

When a function is called, the compiler creates a **stack frame** — a
region on the stack holding that function's saved registers and local
variables. Each nested call pushes a new frame; each return pops one:

```
High address (stack grows downward)
+-------------------------+
| worker's frame          |  ← worker's sp on entry
|   saved ra, s0-s11      |
|   local vars (count)    |
+-------------------------+
| kprintf's frame         |  ← kprintf's sp on entry
|   saved ra, ...         |
+-------------------------+
          ...
Low address
```

Two registers manage this:

- **`sp` (x2)** — stack pointer. Points to the current top (lowest
  used address). Moves on every function call/return.
- **`s0` / `fp` (x8)** — frame pointer. Points to a fixed location
  in the current frame. Gives a stable base address for accessing
  local variables when `sp` moves unpredictably within a function.

  > **When is `sp` unpredictable?** Almost never in kernel code. `sp`
  > moves by a compile-time constant in the prologue and stays put —
  > the compiler knows every offset. The rare exceptions: `alloca()`
  > (allocates on the *stack*, not heap like `malloc`) and C99
  > variable-length arrays (`int arr[n]` with runtime `n` — added in
  > C99, made optional in C11, banned in the Linux kernel, and avoided
  > in most professional C code due to silent stack overflow risk and
  > poor portability across compilers). In our kernel, `sp` is always
  > deterministic.
  >
  > Since `sp` is deterministic, **`-O2` eliminates the frame pointer
  > entirely** — the compiler tracks everything relative to `sp` and
  > frees `s0` as a general-purpose callee-saved register. This is why
  > `s0` often holds cached values like `&cpus` in our worker function
  > rather than serving as a frame pointer.

### The two sides of register saving

Register saving is split between two responsibilities:

**Callee side (inside the function body):** The function saves any
callee-saved registers (`s0–s11`, `ra`) that its body will overwrite,
and restores them before `ret`. Only the registers it actually uses
are saved — the compiler is lazy in the best sense.

```asm
# Callee: function prologue/epilogue
foo:
    addi  sp, sp, -32      # allocate frame
    sd    ra, 24(sp)       # save ra — foo calls bar below, and `call bar` overwrites ra
    sd    s0, 16(sp)       # save s0 (this function will use it)
    sd    s1, 8(sp)        # save s1 (this function will use it)
    ...                    # function body — free to use s0, s1
    ld    s1, 8(sp)        # restore s1
    ld    s0, 16(sp)       # restore s0
    ld    ra, 24(sp)       # restore ra
    addi  sp, sp, 32       # deallocate frame
    ret
```

**Caller side (around a `call` instruction):** Before calling another
function, the caller saves any caller-saved registers (`t0–t6`,
`a0–a7`) that it still needs after the call returns. Again, only the
ones that are live (still needed later) — dead registers are not saved.

```asm
# Caller: saving around a call
    ...                    # computing with t0 — still need it after call
    sd    t0, 0(sp)        # save caller-saved reg we need later
    call  bar              # bar may trash t0
    ld    t0, 0(sp)        # restore — bar might have clobbered t0
    ...                    # continue using t0
```

**Neither side saves registers it doesn't need.** The compiler analyzes
liveness:
- Callee: "which s-regs will my body overwrite?" → save only those
- Caller: "which t/a-regs do I still need after this call?" → save
  only those

So a stack frame's size varies per function based on how many registers
actually need saving plus how many local variables can't live in
registers.

### Function parameters: where do they live?

The calling convention places the first 8 arguments in `a0–a7`. What
happens to them inside the function depends on usage:

| Situation | Where parameter lives |
|---|---|
| Read-only, no call crosses | stays in `a0–a7` — never touches memory |
| Needed across a call | moved to an `s`-reg, or spilled to stack |
| Address taken (`&param`) | forced to stack (registers have no address) |

**Example — parameter stays in register (no call, read-only):**
```c
int add(int x, int y) { return x + y; }
```
```asm
add:
    add  a0, a0, a1       # use a0/a1 directly
    ret                   # no stack frame at all!
```

**Example — parameter needed across a call:**
```c
int foo(int x) { bar(); return x + 1; }
```
The compiler can either spill `a0` to the stack, or (better) move it
to a callee-saved register that survives the call:
```asm
foo:
    addi sp, sp, -16
    sd   ra, 8(sp)
    sd   s0, 0(sp)        # save old s0 (callee obligation)
    mv   s0, a0           # stash parameter in s0 — survives call to bar
    call bar
    addi a0, s0, 1        # use s0 directly, no memory reload
    ld   s0, 0(sp)        # restore old s0
    ld   ra, 8(sp)
    addi sp, sp, 16
    ret
```

**Example — address taken (must be in memory):**
```c
int foo(int x) { return bar(&x); }
```
Here `x` MUST go to the stack — registers don't have addresses.

> **Textbook vs reality:** University courses teach "parameters go on
> the stack as local variables." That's the C *abstract machine* model.
> The compiler optimizes away from that whenever it can prove the address
> is never taken and the value fits in a register. With `-O0` (no
> optimization), everything goes to the stack — which is why debugging
> is easier at `-O0` (GDB can inspect every variable's memory address).

### The key point for Bug 2

**Each function overwrites `s0` with its own value** (frame pointer or
cached data). This is safe normally because `s0` is callee-saved —
each function saves the old value in its prologue and restores it in
its epilogue. But if we bypass the epilogue (via `swtch`), the old `s0`
is lost unless we saved it elsewhere (kernel_vec's stack frame).

### Bug 1: Process never gets preempted

**Symptom:** Worker A runs forever. Timer is armed by the scheduler, but
worker A never yields — it prints hundreds of lines without interruption.

**Root cause:** When a trap fires, hardware sets `SIE = 0` (disabling
further interrupts). Normally `sret` restores SIE from SPIE. But we
leave the trap via `swtch` instead of `sret` — so SIE stays 0. The
next process inherits this disabled state.

**Trace:**

```
kmain sets SIE = 1
  → scheduler picks idle → swtch to idle
  → idle: wfi → timer fires
  → hardware: SPIE = SIE(1), SIE = 0   ← interrupts disabled
  → kernel_vec → kernel_trap → yield → swtch to scheduler
    (SIE is still 0 — we left via swtch, not sret)
  → scheduler picks worker_a → swtch to worker_a
  → worker_a runs with SIE=0
  → timer fires in M-mode, SSIP is set, but S-mode never takes
    the interrupt because SIE=0!
```

The `sret` that would restore SIE is frozen on idle's stack — it won't
run until idle is scheduled again.

**Fix:** Each kernel thread enables interrupts at its start:

```c
void worker(void) {
    intr_on();  /* SIE = 1 */
    ...
}
```

New threads enter via `swtch → ret` (not via `sret`), so there's no
hardware mechanism to restore SIE. They must do it manually. Once a
thread has been preempted and later resumes, the full path runs:
`swtch → yield returns → kernel_trap returns → kernel_vec → sret`
— and `sret` restores SIE from SPIE (which was 1 when the trap was
originally taken). So the manual `intr_on()` is only needed once, at
the very first entry.

### Bug 2: Callee-saved registers corrupted after preemption

**Symptom:** After the timer preempts a process and it resumes, register
`s0` (which the compiler uses to hold `&cpus`) is 0 → NULL dereference
→ page fault.

**Root cause:** `kernel_vec.S` originally only saved **caller-saved**
registers (ra, t0-t6, a0-a7). The reasoning was: "kernel_trap is a
normal C function, it preserves s0-s11 per the calling convention."

This was correct before scheduling existed. But now `kernel_trap` calls
`yield()` → `swtch()`, which switches to another process. The other
process runs with its own s0-s11 values. When we eventually switch back,
`swtch` restores s0-s11 from context — but these are the values from
inside `yield()` (kernel_trap's frame pointer, etc.), NOT the worker's
original s0-s11.

**Trace of the corruption:**

```
Worker running: s0 = &cpus (compiler cached this address in s0)
  → timer fires
  → kernel_vec saves ONLY caller-saved regs (s0 NOT saved to stack!)
  → kernel_trap prologue: saves old s0, sets s0 = its own frame pointer
  → kernel_trap body runs (s0 = kernel_trap's fp, NOT &cpus)
  → yield → swtch: saves s0 into context (= kernel_trap's fp)
  → ... other process runs ...
  → swtch back: restores s0 from context (= kernel_trap's fp)
  → yield returns → kernel_trap epilogue restores s0...
    BUT restores to what? kernel_trap saved the s0 it received from
    its caller (kernel_vec). kernel_vec set s0? No — kernel_vec is
    assembly, it didn't touch s0. So kernel_trap's saved s0 is
    whatever s0 was on trap entry = &cpus. Epilogue restores it...

    Wait — actually this SHOULD work? Let's look more carefully.
```

The subtlety: `kernel_trap`'s prologue saves the s0 it received (which
is the worker's `&cpus`, passed through from kernel_vec). Its epilogue
restores it. So after `kernel_trap` returns, s0 = &cpus again — IF the
function returns normally.

But with `swtch` in the middle: `kernel_trap` calls `yield()`, which
calls `swtch`. `swtch` saves s0 — at this point s0 is `yield()`'s
frame pointer (which `yield` set in its own prologue). When we resume
via swtch, s0 is restored to yield's fp. Yield returns to kernel_trap.
kernel_trap's epilogue restores s0 from *its* stack frame — which holds
the original &cpus. Then kernel_trap returns to kernel_vec.

So the callee-save chain *does* properly unwind... UNLESS another
process's `swtch` restores different s0-s11 values and then those leak
into our kernel_vec frame. Let's trace what happens to kernel_vec's
caller-saved-only approach:

```
Worker: s0 = &cpus
  → timer fires
  → kernel_vec: saves ra, t0-t6, a0-a7 to stack. Does NOT save s0.
  → call kernel_trap
  → kernel_trap prologue: sd s0, N(sp)  ← saves &cpus to ITS frame
  → kernel_trap sets s0 = its own frame pointer
  → calls yield() → yield sets s0 = yield's frame pointer
  → swtch(&A.context, &scheduler): saves s0 = yield's fp into A.context

  ... scheduler picks process B ...
  ... B runs, eventually yields ...

  → swtch(&scheduler, &A.context): restores s0 = yield's fp (correct!)
  → yield epilogue: restores s0 from yield's stack = kernel_trap's fp
  → kernel_trap epilogue: restores s0 from kernel_trap's stack = &cpus ✓
  → kernel_trap returns to kernel_vec
  → kernel_vec: does NOT restore s0 (didn't save it)
  → sret: s0 = &cpus ← THIS SHOULD WORK!
```

Hmm — the chain seems correct. But in practice it crashed. Why?

The issue is **compiler optimization**: with `-O2`, the compiler may NOT
use a frame pointer. Instead, it accesses locals relative to `sp` only.
`s0` becomes just another callee-saved register used for arbitrary
purposes (like caching `&cpus`). When `kernel_trap` is compiled without
a frame pointer, it might use s0 for something else entirely (a loop
variable, a temporary) and not save/restore it if the compiler proves
it's not needed across the call to `yield`.

More precisely: the compiler sees that `kernel_trap` calls `yield()`
which calls `swtch` (an external assembly function). `swtch` is declared
to save/restore s0-s11 — so from the compiler's perspective, s0 is
preserved across the `yield()` call. The compiler trusts that. But
`swtch` saves s0 into one process's context and restores s0 from a
DIFFERENT process's context — the compiler doesn't know this.

The result: kernel_trap might skip saving s0 to its stack frame because
it "knows" yield/swtch preserves it. But swtch swaps it with another
process's value. When kernel_trap returns, s0 has the wrong value.
kernel_vec doesn't save/restore s0 either. Crash.

**Fix:** `kernel_vec.S` must save ALL general-purpose registers (including
s0-s11). This makes the save/restore independent of what the compiler
decides to do with callee-saved registers inside kernel_trap:

```
kernel_vec saves ALL regs (including s0 = &cpus) to stack
  → kernel_trap runs (may or may not save s0 to its own frame)
  → yield → swtch → other process → swtch back
  → kernel_trap returns (s0 = unknown/wrong value)
kernel_vec restores ALL regs from stack (s0 = &cpus — correct!)
  → sret
```

kernel_vec's save is the **authoritative copy** of the worker's register
state. Whatever happens between save and restore — function calls,
context switches, compiler optimizations — doesn't matter. The stack
frame captures the exact moment of interruption.

### Bug 3: kprintf corruption under preemption

**Symptom:** With bugs 1 and 2 fixed, timer preemption works — processes
interleave. But after a few switches, output shows `[] 0` (empty name,
zero count) and eventually crashes with a page fault at a small
non-zero address (corrupted struct pointer).

**Root cause:** `kprintf` is not reentrant. It uses shared state
internally (format string parsing position, output buffer pointer). When
the timer fires mid-kprintf (process A is halfway through printing),
the scheduler switches to process B, which also calls kprintf. Both
processes are now interleaving inside the same kprintf instance,
corrupting each other's state.

**Trace:**

```
Worker A: kprintf("[worker_a] pid=1 co
  → timer fires mid-print
  → kernel_vec → kernel_trap → yield → scheduler → worker B
Worker B: kprintf("[worker_b] pid=2 count=1\n")
  → this completes, but kprintf's internal state was left
    mid-string by A — B's output may be garbled, and A's
    saved format pointer is now stale
  → timer fires → scheduler → worker A resumes
Worker A: ...unt=1\n") — but internal state is corrupted
  → reads from wrong address → page fault
```

**Fix:** Disable interrupts around kprintf calls:

```c
if (count % 1000000 == 0) {
    intr_off();
    kprintf("[%s] pid=%d count=%d\n", ...);
    intr_on();
}
```

This ensures the timer cannot preempt a process mid-print. The timeslice
may be slightly extended (by the duration of one print), but correctness
is guaranteed. This is the same approach Linux uses — `printk` acquires
a console lock that prevents preemption during output.

> **Why kprintf specifically?** Any function can be safely preempted as
> long as it only touches **private state** (stack variables, registers).
> The kernel correctly saves/restores all of these across a context
> switch. kprintf is vulnerable because it touches a **shared resource**
> — the UART device register. Both processes write to the same physical
> UART, one byte at a time. If A is mid-output and B starts printing,
> the device receives interleaved bytes. Worse, the crash occurs because
> the format string and `va_list` traversal can end up with stale
> pointers when the preemption happens at exactly the wrong moment.
>
> Pure computation (`count++`, arithmetic, comparisons) is immune — it
> only uses registers and stack, which are per-process and correctly
> saved/restored. The rule: **preemption is safe if and only if no
> shared mutable state is accessed.** When shared state is involved,
> you need either `intr_off` (disable preemption) or a lock (future).
>
> Note: the `ret_from_trap` design does NOT fix this bug — it only
> ensures kernel_trap itself isn't preempted. kprintf runs in process
> context (not trap context), so it can still be interrupted by a timer
> that triggers ret_from_trap → yield on the return path.

### Summary: lessons from the bugs

| Bug | Cause | Category |
|-----|-------|----------|
| No preemption | SIE=0 inherited from frozen trap | yield-inside-trap side effect |
| s0 corruption | kernel_vec didn't save callee-saved regs | yield-inside-trap side effect |
| kprintf crash | Non-reentrant code preempted mid-execution | Missing critical section |

All three stem from the same architectural choice: **calling yield()
inside the trap handler.** The trap frame is "suspended" mid-execution
while other processes run, creating:
- Register state that the normal return path (`sret`) would have
  restored, but `swtch` bypasses
- Shared code (kprintf) that assumes single-threaded execution,
  violated by preemption

Production OSes (Linux, FreeBSD, Windows) avoid these issues by
deferring scheduling: the trap handler just sets a `need_resched`
flag and returns via `sret` (which properly restores all state). The
actual context switch happens later at a well-defined scheduling point.
xv6 (and we) accept the complexity for simplicity of implementation.

### The better design: `ret_from_trap`

The fundamental constraints that make "swtch before sret" unavoidable:
1. Only this hart can save its own registers (no instruction to read
   another hart's registers remotely)
2. Registers must be saved before switching to another process
3. swtch must happen before sret — otherwise the process resumes and
   the scheduling opportunity is lost

Given these constraints, the question isn't *whether* to switch inside
the kernel_vec frame, but *how clean the call stack is* when we switch.

**Current approach (yield inside kernel_trap):**

```
B's kernel stack (frozen when preempted):
+---------------------------+ ← kstack + PG_SIZE (top)
| kernel_vec frame (256B)   |   all 32 regs saved
+---------------------------+
| kernel_trap frame         | ← SUSPENDED mid-execution
+---------------------------+
| yield frame               |
+---------------------------+ ← B's context.sp
```

kernel_trap's frame is live — its locals, frame pointer, everything
still allocated. This is what caused our bugs: kernel_trap's s0 leaks
into the restore path, SIE state is trapped inside the frozen frame,
and any non-reentrant code (kprintf) in kernel_trap can be preempted.

**Better approach (ret_from_trap):**

```c
// kernel_trap: just handle the event, set a flag
void kernel_trap(void) {
    ...
    if (timer_interrupt)
        this_cpu()->need_resched = 1;  // don't yield here!
    ...
}

// ret_from_trap: separate scheduling decision point
void ret_from_trap(void) {
    if (this_cpu()->need_resched) {
        this_cpu()->need_resched = 0;
        yield();
    }
}
```

```asm
kernel_vec:
    save all regs
    call kernel_trap       # handles event, returns cleanly
    call ret_from_trap     # checks flag, maybe yields
    restore all regs
    sret
```

The frozen stack becomes:

```
B's kernel stack (frozen when preempted via ret_from_trap):
+---------------------------+ ← kstack + PG_SIZE (top)
| kernel_vec frame (256B)   |   all 32 regs saved
+---------------------------+
| ret_from_trap frame       |   tiny (just ra)
+---------------------------+
| yield frame               |
+---------------------------+ ← B's context.sp
```

kernel_trap's frame is **gone** — it returned normally, prologue/epilogue
ran, all its callee-saved registers restored by the compiler. No
suspended C function on the stack.

**Three layers, each with a single responsibility:**

| Layer | Responsibility | Knows about scheduling? |
|-------|---------------|------------------------|
| kernel_vec | Save/restore registers | No |
| kernel_trap | Handle the event, set flags | Only sets a flag |
| ret_from_trap | Policy decision before returning | Yes — checks flag, calls yield |

**Why the bugs disappear:**
- **SIE bug:** kernel_trap returns normally, no frozen trap state. Can
  enable SIE in ret_from_trap before yield, or just rely on sret
  restoring SPIE after yield returns.
- **Register corruption:** kernel_trap finished — its frame is
  deallocated. s0-s11 are whatever kernel_trap's epilogue restored
  (which is what kernel_vec saved). yield/swtch operate on a clean
  state.
- **kprintf reentrancy:** kernel_trap runs to completion without being
  switched away. No mid-function preemption.

This is the Linux/FreeBSD model (`ret_from_exception`, `ast()`). After
debugging the issues with yield-inside-trap (Part 7 above), we refactored
bobchouOS to adopt this cleaner design.

---

## Quick Reference

### Process states (Round 5-1)

| State | Meaning | In run queue? |
|-------|---------|---------------|
| `PROC_RUNNABLE` | Ready to run, waiting for CPU | Yes |
| `PROC_RUNNING` | Currently executing on a hart | No (it's running) |

### Key functions

| Function | Purpose |
|----------|---------|
| `proc_init()` | Initialize run queue, PID table, CPU struct |
| `proc_create_kernel(fn, name)` | Allocate proc, set up context, add to run queue |
| `scheduler()` | Loop: pick next runnable, swtch to it |
| `yield()` | Mark RUNNABLE, enqueue, swtch to scheduler |
| `swtch(old, new)` | Save 14 regs to old, restore from new, ret |
| `this_cpu()` | Return current hart's `struct cpu` |
| `this_proc()` | Return currently running process |
| `intr_on()` / `intr_off()` | Enable/disable S-mode interrupts (SIE bit) |

### `struct context` layout (byte offsets)

| Offset | Register | Role |
|--------|----------|------|
| 0 | ra | Resume address |
| 8 | sp | Stack pointer |
| 16 | s0 | Frame pointer |
| 24 | s1 | Callee-saved |
| 32–104 | s2–s11 | Callee-saved |
