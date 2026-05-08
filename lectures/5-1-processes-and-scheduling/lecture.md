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
> tackle user-mode processes in Round 5-2.
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

```
                    +-----------+
         +--------->|  RUNNABLE |<---------+
         |          +-----+-----+          |
         |                |                |
         |          schedule picks it   yield / preempt
         |                |                |
         |                v                |
         |          +-----------+          |
         |          |  RUNNING  |----------+
         |          +-----+-----+
         |                |
    wake up        exit() |-----sleep()
         |                |          |
         |                v          v
         |          +---------+  +-----------+
         |          | ZOMBIE  |  |  SLEEPING |
         |          +---------+  +-----+-----+
         |                             |
         +-----------------------------+
```

For Round 5-1, we only need **RUNNABLE** and **RUNNING**. SLEEPING and
ZOMBIE arrive in Round 5-3 with `sleep()`/`exit()`/`wait()`.

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

    /* --- Family (Round 5-3) --- */
    struct proc *parent;
    struct list_head children;    // head of my children list
    struct list_head sibling;     // link in parent's children list

    /* --- Execution state --- */
    struct context context;       // saved callee-regs for swtch
    uint64 kstack;                // kernel stack base address

    /* --- Address space (Round 5-2) --- */
    pagetable_t pagetable;        // user page table
    struct trapframe *trapframe;  // saved user regs for trap
    uint64 sz;                    // user memory size

    /* --- Exit (Round 5-3) --- */
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
- **User stack** — (Round 5-2) used when running in U-mode. Saved
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
stack). The two-stack model becomes visible in Round 5-2.

### `struct cpu` — per-hart state

Even with one hart, we need a place to store the scheduler's own
context and a pointer to the currently running process:

```c
struct cpu {
    struct proc *proc;            // currently running process, or NULL
    struct context scheduler;     // scheduler's saved context
    int noff;                     // interrupt-disable nesting depth
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
(Round 5-2 concerns). Kernel threads aren't just for testing — real
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

### `struct context` layout (byte offsets)

| Offset | Register | Role |
|--------|----------|------|
| 0 | ra | Resume address |
| 8 | sp | Stack pointer |
| 16 | s0 | Frame pointer |
| 24 | s1 | Callee-saved |
| 32–104 | s2–s11 | Callee-saved |
