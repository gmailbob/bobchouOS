# Lecture 5-2: Process Lifecycle — Spinlocks, Sleep/Wakeup, Exit & Wait

> **Where we are**
>
> Round 5-1 gave us the scheduling mechanism: `struct proc`, `swtch()`,
> the scheduler loop, timer preemption. Kernel threads run, interleave,
> and yield. But they can never *end*. There is no way for a process to
> say "I'm done" and have its resources reclaimed. There is no way to
> wait for something to happen without busy-spinning. There is no way
> to safely modify shared data when interrupts could fire at any moment.
>
> This lecture completes the process story. By the end, processes have a
> full lifecycle: they are created, they run, they sleep waiting for
> events, they exit, and they are reaped by their parent. The kernel
> can kill them. An init process sits at the root of the tree, adopting
> orphans and reaping zombies forever.
>
> But first, we need synchronization — because none of the above is safe
> without locks.
>
> **What you will understand after this lecture:**
>
> - What a process IS (precise definition, not just "running program")
> - Process creation and the parent-child contract
> - Spinlocks — atomic test-and-set, memory ordering, interrupt safety
> - Wait queues — efficient targeted wakeup using our list.h
> - Sleep/wakeup — the lost wakeup problem and its solution
> - The golden rule of process state locking
> - exit(), wait(), kill() — the full lifecycle
>
> **xv6 book coverage:** Chapter 6 sections 6.1–6.3 (races, spinlocks,
> using locks), 6.6 (locks and interrupts). Chapter 7 sections 7.5–7.6
> (sleep and wakeup), 7.8 (wait, exit, kill), 7.9 (process locking).
> Chapter 6.8 (sleep-locks / mutexes) is deferred to Phase 7.

---

## Part 1: What Is a Process? (Precise Definition)

Lecture 5-1 introduced processes informally: "a running instance of a
program." That's good intuition but imprecise. Let's nail it down.

### The definition

A **process** is the kernel's unit of resource ownership and scheduling.
It is defined by three things:

1. **An execution context** — the CPU state (registers, program counter,
   stack pointer) that determines what instruction runs next and what
   data is accessible. When a process is not running, this context is
   saved in `struct context` (callee-saved regs + ra) and on its kernel
   stack (caller-saved regs in call frames). Only callee-saved registers
   are in `struct context` because `swtch()` is a normal function call —
   the calling convention guarantees caller-saved regs are already on
   the stack before `swtch` is reached. `ra` is added because it serves
   as the resume address (where to jump on `ret`).

2. **A resource container** — the kernel-managed resources owned by this
   process: its kernel stack, its page table (Phase 6), its open files
   (Phase 7), its PID, its parent-child relationships.

3. **A lifecycle state** — where the process currently is in its journey
   from creation to destruction: runnable, running, sleeping, or zombie.

A process is NOT:
- A program (that's static code on disk)
- A thread (in our model, each process has exactly one thread of
  execution — multi-threading arrives in Phase 9)
- A function (a function is code; a process is code *plus* dynamic state
  *plus* kernel metadata *plus* lifecycle management)

### The lifecycle state machine

```
                            creation
                        (proc_create_kernel
                         or fork in Phase 6)
                                |
                                v
    +-------------------->[RUNNABLE]<---------------------+
    |                           |                         |
    |                     scheduler picks                 |
    |                           |                      wakeup()
    |                           v                         |
    |                       [RUNNING]                     |
    |                      /    |    \                    |
    |             yield() /   exit()   sleep()            |
    |             preempt()     |          |              |
    |                  |        |          v              |
    +------------------+     [ZOMBIE]   [SLEEPING]--------+
                                |
                        parent calls wait()
                                |
                                v
                            (freed)
```

Key transitions:
- **RUNNABLE → RUNNING**: only the scheduler does this
- **RUNNING → RUNNABLE**: `yield()` — voluntary or timer-forced
- **RUNNING → SLEEPING**: `wq_sleep()` — waiting for an event
- **SLEEPING → RUNNABLE**: `wq_wake_one/all()` — event occurred
- **RUNNING → ZOMBIE**: `exit()` — process is done, waiting to be reaped
- **ZOMBIE → freed**: parent's `wait()` — frees kstack and proc struct

There is no transition FROM ZOMBIE to anything. A zombie never runs
again. There is no transition from SLEEPING to ZOMBIE directly — a
killed sleeping process is woken up (marked RUNNABLE), runs briefly,
notices `p->killed`, and calls `exit()` itself.

### Why no CREATED / UNUSED state?

xv6 has `UNUSED` (slot available in fixed array) and `USED` (allocated
but not yet runnable). We don't need either:

- **No UNUSED**: we kmalloc each proc dynamically — no fixed array
- **No USED/CREATED**: `proc_create_kernel()` atomically sets up the
  proc and marks it RUNNABLE in one operation. There's no window where
  a half-initialized proc is visible to the scheduler.

Four states suffice: RUNNABLE, RUNNING, SLEEPING, ZOMBIE.

---

## Part 2: Process Creation and the Parent-Child Contract

### How processes are born

Across all operating systems, there are only two ways to create a process:

| Method | Who uses it | Mechanism |
|--------|-------------|-----------|
| **Direct creation** (function pointer) | Kernel creating kernel threads | Allocate proc + kstack, set context.ra = function, mark RUNNABLE |
| **Duplication** (fork) | User process creating child | Copy parent's address space + trapframe, child "returns from fork" with value 0 |

Our kernel currently has only the first: `proc_create_kernel(fn, name)`.
fork arrives in Phase 6 as a user-mode syscall.

**No modern OS forks kernel threads:**

| OS | Kernel thread creation |
|----|----------------------|
| Linux | `kthread_create(function, data, name)` |
| FreeBSD | `kthread_add(function, arg, ...)` |
| Windows | `PsCreateSystemThread(function, ...)` |
| bobchouOS | `proc_create_kernel(function, name)` |

All take a function pointer. fork doesn't apply to kernel threads
because: (1) they share one page table — nothing to duplicate, (2)
copying a kstack breaks all frame pointers and saved register
references, (3) there's no use case — kernel threads run defined
functions, not "clone myself and diverge."

### Why every process needs a parent

The process cannot free itself on exit — it's still running on its own
kstack. This asymmetry ("I can die but I can't bury myself") means
**someone else** must clean up. Unix's answer: the parent does it via
`wait()`.

This creates a contract:
- You create a child → you're responsible for reaping it
- If you die first → your orphans are reparented to init (who always waits)
- If you never call wait → zombies accumulate (memory leak)

This parent-reaps-child model is universal across Unix-family systems.
PID 1 (the universal reaper) has different names depending on the OS:

| OS | Lineage | PID 1 | Process cleanup |
|---|---|---|---|
| Linux (modern) | Unix-inspired | `systemd` | Parent calls wait(), zombies, systemd reaps orphans |
| Linux (older) | Unix-inspired | `/sbin/init` (SysVinit) | Same model |
| FreeBSD | BSD (direct Unix descendant) | `/sbin/init` | Same model |
| macOS | Mach microkernel + FreeBSD userland | `launchd` | Same model |
| Docker containers | Linux | whatever `ENTRYPOINT` is | PID 1 exit = container stops |
| bobchouOS | — | `init_thread` (kernel) | Same model (user init in Phase 6) |
| Windows | NT kernel (VMS heritage) | *No PID 1* | Handle-based refcounting, no parent-child reaping |

> **Windows's handle-based model:** Windows has no zombies, no
> reparenting, no universal reaper. Instead:
>
> | | Unix/Linux | Windows |
> |---|---|---|
> | Who cleans up? | Parent (via wait) | Whoever closes the last handle |
> | Exit status | Parent retrieves via wait() | Anyone with a handle calls `GetExitCodeProcess()` |
> | Zombie state? | Yes — until parent reaps | No — process object lives until all handles closed |
> | Parent dies first? | Reparent to init | No problem — handles are independent of parent |
>
> `CreateProcess()` returns a **handle** to the new process. The process
> object stays alive as long as any handle exists (parent, debugger, job
> object, anyone). When the last handle is closed (`CloseHandle()`), the
> kernel frees the process object. The creator doesn't have a special
> obligation — it can close its handle immediately and the child runs
> independently.
>
> Why Unix didn't do this: handles add complexity (reference counting,
> handle tables, security descriptors). Unix V6 (1975) was designed for
> simplicity — "parent owns child" is the simplest possible ownership
> model.

### The two bootstrap processes

PID 0 and PID 1 are special — they have no parent and are created
directly by the kernel during boot:

| | PID 0 (idle) | PID 1 (init) |
|---|---|---|
| Purpose | Keep CPU occupied when nothing is runnable | Root of process tree, universal reaper |
| What it does | `wfi` + `yield` loop | `wait()` in a loop forever |
| Has children? | No | Yes — all other processes |
| Can exit? | No — system hangs | No — kernel panics |
| Sleeps? | **Never** — always RUNNABLE or RUNNING | **Mostly** — SLEEPING inside `wait()` |

Idle is the guaranteed scheduler fallback because it **never sleeps**.
It only calls `wfi` + `yield`, never `wq_sleep`. Init spends most of
its time SLEEPING (off the run queue, invisible to the scheduler).

**Why idle needs both `wfi` and `yield`:**
- `wfi` only — correct but slow: newly-RUNNABLE processes wait up to
  10ms for the timer to kick idle off the CPU.
- `yield` only — correct but wasteful: hot loop burning power.
- `wfi` + `yield` — `wfi` saves power, any interrupt wakes the CPU,
  `yield` immediately gives the scheduler a chance to pick new work.

### The boot picture

```c
void kmain(void) {
    // Hardware + memory init...
    proc_init();

    proc_create_kernel(idle_thread, "idle");   // PID 0 — scheduler fallback
    proc_create_kernel(init_thread, "init");   // PID 1 — process tree root
    proc_create_kernel(worker, "worker_a");    // PID 2 — child of init
    proc_create_kernel(worker, "worker_b");    // PID 3 — child of init

    scheduler();  // never returns
}
```

---

## Part 3: Spinlocks

### Why we need locks now

In Round 5-1, we got away without locks because:
1. Only one hart — no true parallelism
2. Interrupts during critical sections were "fixed" by disabling them
   around kprintf

But now we need to protect *process state* during transitions. Consider:

```c
// What if a timer interrupt fires right here?
p->state = PROC_RUNNABLE;
// Timer → kernel_trap_ret → yield → scheduler sees p is RUNNABLE
// and tries to run it — but we haven't added it to the run queue yet!
list_add_tail(&p->run_list, &run_queue);
```

Disabling interrupts around every state change works for single-hart,
but doesn't scale to multi-hart (Phase 9) and doesn't compose well
with sleep/wakeup. We need real locks.

### What a spinlock is

A spinlock is the simplest possible mutual exclusion primitive:
- **Acquire**: atomically try to set a flag from 0→1. If it was already
  1, spin (loop) until it becomes 0, then try again.
- **Release**: set the flag back to 0.
- **Invariant**: while you hold the lock, no one else can acquire it.

The key is "atomically" — you must read and write the flag in a single
indivisible operation. Otherwise two CPUs could both read 0 and both
set 1, thinking they both acquired it.

### RISC-V atomic instruction: `amoswap`

RISC-V provides `amoswap.w` (Atomic Memory Operation: Swap, Word):

```asm
amoswap.w  t0, t1, (a0)
# Atomically: t0 = *a0; *a0 = t1;
# One indivisible operation — no interrupt or other CPU can slip between
# the read and the write.
```

This does in ONE atomic step what would otherwise take two (load + store)
that could be interrupted or interleaved by another hart.

### How the spinlock loop works

The GCC built-in `__sync_lock_test_and_set(&lk->locked, 1)` compiles to
`amoswap`. The name is misleading — it doesn't "test if setting
succeeds." It **always performs the swap unconditionally** and **returns
the old value**:

```c
int old = __sync_lock_test_and_set(&lk->locked, 1);
// Atomically: old = lk->locked; lk->locked = 1;
// The swap ALWAYS happens. The return value tells you what was there before.
```

| Before swap | After swap | Returns | Meaning |
|---|---|---|---|
| `locked = 0` | `locked = 1` | **0** | "Was free → now I hold it" |
| `locked = 1` | `locked = 1` | **1** | "Was held → I wrote 1 over 1 (no-op), try again" |

The acquire loop:
```c
while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;
```

Reads as: "keep swapping 1 in. If the old value was 0, I won the lock
(exit loop). If it was 1, someone else has it (loop, try again)."

On multi-hart, two CPUs can execute `amoswap` simultaneously on the
same address — but the hardware serializes them (only one sees the
0→1 transition, the other sees 1→1):

```
Hart 0:                          Hart 1:
  amoswap → old=0, set=1          amoswap → old=1, set=1
  (won! exit loop)                 (lost — old was 1, keep spinning)
  ... critical section ...         amoswap → old=1, loop...
  release: locked = 0              amoswap → old=0, set=1
                                   (won! exit loop)
```

### Advisory vs mandatory protection

Spinlocks are **advisory** — they're a programmer's convention, not
hardware enforcement. Nothing stops you from writing:

```c
p->state = PROC_ZOMBIE;    // no lock held — compiles fine, runs fine, silently wrong
```

The hardware doesn't know that `p->state` is "supposed to" be protected
by `p->lock`. There's no fault, no trap. It just silently corrupts state
— and the bug might not manifest until thousands of cycles later.

Compare to the mandatory protections we've already built:

| Mechanism | Advisory or mandatory? | Enforced by |
|---|---|---|
| Spinlocks | Advisory — programmer must use them | Nothing (convention only) |
| Page tables (PTE_U) | Mandatory — U-mode can't access kernel pages | Hardware MMU (trap on violation) |
| PMP | Mandatory — S-mode can't access M-mode memory | Hardware PMP unit (trap on violation) |

But `amoswap` itself IS mandatory — **hardware-guaranteed atomic**:
- On single-core: no interrupt can fire between the read and write
- On multi-core: the cache coherence protocol locks the cache line
  during the operation — no other core can read or write that address
  until it completes

The hierarchy — advisory built on top of mandatory:

```
Advisory (software convention):
    spinlock        → built on amoswap
    mutex           → built on spinlock + sleep/wakeup
    wait_lock, etc. → all spinlocks

Mandatory (hardware enforced):
    amoswap         ← the foundation (cache line locked during operation)
    page tables     ← MMU traps on violation
    PMP             ← traps on violation
```

Without hardware atomics, you cannot build correct locks on multi-core.
That's why RISC-V has the "A" extension (Atomic) — our `rv64imac`
includes it.

### Memory ordering: why atomicity alone isn't enough

Atomicity guarantees the swap itself is indivisible. But there's a
second, subtler problem: **memory reordering**. Both the compiler and
the CPU hardware can reorder memory operations for performance.

**Compiler reordering:** The C compiler rearranges loads/stores if it
can't see a data dependency:

```c
spin_lock(&lk);       // sets lk->locked = 1
x = 42;              // the compiler might move this BEFORE the lock!
spin_unlock(&lk);
```

The compiler sees no dependency between `lk->locked` and `x` — they're
different memory locations. So it's free to reorder them.

**Hardware reordering:** Even if the compiler emits instructions in the
correct order, the CPU's pipeline may execute loads/stores out of order.
On a multi-core system, CPU A's store to `x` might remain in A's store
buffer while CPU B sees the lock released — B grabs the lock and reads
a stale `x`.

### Fences: controlling memory order

RISC-V provides the `fence` instruction to enforce ordering:

```asm
fence rw,rw
# "All reads/writes before this instruction must complete before
#  any reads/writes after this instruction begin."
```

The two arguments specify which operations to order:
- First argument: what must complete (r=reads, w=writes, rw=both)
- Second argument: what must wait (r=reads, w=writes, rw=both)

RISC-V also provides ordering suffixes on atomic instructions:
- **`.aq` (acquire)**: no later memory operation can be reordered
  *before* this instruction.
- **`.rl` (release)**: no earlier memory operation can be reordered
  *after* this instruction.

These are **weaker** than a full `fence rw,rw` — each only blocks
movement in one direction (which may or may not be cheaper depending
on the microarchitecture, but is never stronger):

| Instruction | Rule | Strength |
|---|---|---|
| `fence rw,rw` | Nothing crosses in **either** direction | Full barrier |
| `.aq` (acquire) | Later operations cannot move **before** this point | One-way |
| `.rl` (release) | Earlier operations cannot move **after** this point | One-way |

A full `fence rw,rw` is equivalent to `.aq` + `.rl` combined — it
blocks both directions. But remember: ordering and atomicity are
**separate guarantees**. `.aq`/`.rl` only provide ordering — they must
be attached to an atomic instruction (like `amoswap`) that provides
the indivisibility. A `fence` alone cannot replace `amoswap` because
it doesn't make the read-and-write atomic. Conversely, a plain
`amoswap` without `.aq` is atomic but doesn't prevent reordering.

Together, acquire and release contain the critical section:

```
─── nothing from below can move above this line ───
        amoswap.w.aq (lock acquire)     ← acquire ordering
             x = 42;
             y = x + 1;
        fence rw,w                      ← all writes above guaranteed visible
                                        ← (gap — the compiler/CPU may place other
                                           instructions here; that's fine because
                                           they execute after the fence and the
                                           lock is still held until sw executes)
        sw zero, locked                 ← lock appears free (other CPUs see this
                                           only after they can see x=42, y=x+1)
```

The acquire at the top prevents critical section code from leaking
upward. The `fence rw,w` at the bottom prevents critical section stores
from leaking downward past the release.

> **Why `fence rw,w` + `sw zero` instead of `amoswap.w.rl`?** The `.rl`
> suffix can only attach to an atomic instruction like `amoswap`. But
> release doesn't *need* atomicity — only the holder ever writes 0 to
> `locked`, so there's no race on the store itself. A plain `sw` is
> cheaper (no bus lock, no exclusive cache line reservation). The
> `fence rw,w` before it provides the same ordering guarantee. GCC picks
> this cheaper form automatically.

> **`fence` vs `sfence.vma`:** Both use the word "fence" but are
> completely unrelated. `fence` controls memory ordering (store buffers,
> caches). `sfence.vma` flushes the TLB (page table translation cache).
> We used `sfence.vma` in Phase 4 after writing `satp`. They talk to
> different hardware units.

### On single-hart: does this matter?

Hardware reordering doesn't bite on one core — you always see your own
stores in program order. But **compiler reordering** still can. The
GCC built-ins we use act as compiler barriers today; when we add a
second hart in Phase 9, the hardware ordering guarantees become
essential too.

### Implementation

```c
struct spinlock {
    int locked;           // 0 = available, 1 = held
    const char *name;     // for debugging
};

void
spin_lock_irqsave(struct spinlock *lk, unsigned long *flags) {
    *flags = intr_get();  // save current interrupt state
    intr_off();           // disable interrupts

    // Spin until we atomically swap 0→1
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;

    // Full fence: nothing inside critical section leaks before acquire
    __sync_synchronize();
}

void
spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags) {
    // Full fence: everything inside critical section completes before release
    __sync_synchronize();

    // Release: store 0 with release semantics
    __sync_lock_release(&lk->locked);

    if (flags)
        intr_on();        // restore interrupts only if they were on before
}
```

### What the GCC built-ins compile to

| Built-in | RISC-V output | Meaning |
|----------|---------------|---------|
| `__sync_lock_test_and_set(&locked, 1)` | `amoswap.w.aq` | Atomic swap + acquire ordering |
| `__sync_lock_release(&locked)` | `fence rw,w` then `sw zero, (addr)` | Release fence + plain store |
| `__sync_synchronize()` | `fence rw,rw` | Full memory barrier (both directions) |

**Why the extra `__sync_synchronize()` in `spin_lock_irqsave` and
`spin_unlock_irqrestore`?** Two reasons: (1) Older GCC versions may emit
plain `amoswap` without `.aq` for `__sync_lock_test_and_set` — the
explicit fence guarantees ordering regardless of compiler version. (2)
Belt and suspenders — on modern GCC that emits `.aq`, the extra
`fence rw,rw` is strictly redundant but harmless. xv6 includes it for
the same portability reason. One extra fence instruction per lock/unlock
(negligible cost).

> **Why not write inline assembly directly?** The GCC built-ins are
> portable across architectures and the compiler understands their
> semantics (won't reorder around them). Same codegen, better
> maintainability. xv6 uses the same approach.

### Why disable interrupts while holding a spinlock

```
CPU holds spinlock for the run queue
  → timer interrupt fires
  → kernel_trap_ret → yield() tries to acquire run queue lock
  → DEADLOCK — we're spinning waiting for ourselves to release
```

The CPU that holds the lock is the same CPU that took the interrupt.
It will spin forever because it can never return to release the lock.

**Rule: disable interrupts before acquiring any spinlock.**

### Two variants: irqsave vs plain

We provide two pairs of lock functions:

```c
// Variant 1: save/restore interrupt state
// Use at entry points — when you might be the first lock.
void spin_lock_irqsave(struct spinlock *lk, unsigned long *flags);
void spin_unlock_irqrestore(struct spinlock *lk, unsigned long flags);

// Variant 2: plain — no interrupt save/restore
// Use when you KNOW interrupts are already off (inside an irqsave
// block, inside a trap handler, or during boot before interrupts are on).
void spin_lock(struct spinlock *lk);
void spin_unlock(struct spinlock *lk);
```

The plain variant just does the atomic acquire/release without touching
the interrupt state. It's safe only when interrupts are provably off.

**Nested locking in practice:**

```c
// Entry point — first lock, must save interrupt state
unsigned long flags;
spin_lock_irqsave(&wait_lock, &flags);   // flags = 1 (were on), now off

// Inner lock — interrupts already off, use plain variant
spin_lock(&p->lock);
...
spin_unlock(&p->lock);

spin_unlock_irqrestore(&wait_lock, flags); // flags=1, re-enable
```

Only the **outermost** lock uses `irqsave`. Inner locks use the plain
variant — no redundant CSR operations, no unused `flags` variables.

**If you use `irqsave` for nested locks** (also correct, just noisier):

```c
unsigned long flags, flags2;
spin_lock_irqsave(&wait_lock, &flags);    // flags = 1, now off
spin_lock_irqsave(&p->lock, &flags2);     // flags2 = 0 (already off)
...
spin_unlock_irqrestore(&p->lock, flags2); // flags2=0, stay off
spin_unlock_irqrestore(&wait_lock, flags); // flags=1, re-enable
```

Each lock needs its own `flags`. You cannot reuse the same variable —
the second `irqsave` would overwrite the first value (1→0), and both
restores would see 0 (interrupts never re-enabled).

**Rule of thumb:**
- External/public functions → `irqsave` (caller might have interrupts on)
- Internal code already inside a lock → plain `spin_lock`/`spin_unlock`

> **xv6's approach (for reference):** Instead of passing flags around,
> xv6 stores the interrupt state in `struct cpu`: a nesting counter
> (`noff`) and a saved enable bit (`intena`). `push_off()` increments
> the counter; `pop_off()` decrements and restores on last unlock.
> Simpler for the caller (always the same API), but uses hidden
> per-CPU state.

### What spinlocks protect in our kernel

| Lock | Protects | Held by |
|------|----------|---------|
| `wait_lock` | parent pointer, children/sibling lists (the process tree) | wait, exit, reparent |
| `wq->lock` | a specific wait queue's linked list | wq_sleep, wq_wake_one/all |
| `p->lock` | p->state, p->killed, p->exit_status, PID visibility | scheduler, exit, kill, yield |
| `run_queue_lock` | run queue list | run_queue_add helper (self-contained) |
| `pid_lock` | PID counter | alloc_pid (self-contained) |

### Lock ordering

When code needs multiple locks, it must always acquire them in the
same global order. Otherwise:

```
CPU A: lock(wait_lock) → trying lock(p->lock)  ← BLOCKED
CPU B: lock(p->lock)   → trying lock(wait_lock) ← BLOCKED
                         ↑ DEADLOCK ↑
```

Our order (never violated):

```
wait_lock → wq->lock → p->lock
```

`run_queue_lock` and `pid_lock` are not in this chain — they're always
acquired and released within helper functions (`run_queue_add`,
`alloc_pid`), never held while taking another lock.
Self-contained locks don't need ordering.

If you hold `p->lock`, you must NOT try to acquire `wait_lock`. Period.
This is a compile-time discipline — no runtime enforcement (though debug
builds can add checking). The Linux kernel uses lockdep to detect
ordering violations dynamically; we enforce it by convention.

**Release order:** The default convention is reverse of acquisition
(like nested parentheses — last acquired, first released). But this is
a convention for tidiness, not a deadlock rule. **Only acquisition order
prevents deadlock; releasing can never deadlock** (you're giving up
locks, not waiting for them).

In some cases, semantics override the reverse convention. For example,
`wq_sleep` acquires `wq->lock` then `p->lock`, but releases in this
order: condition lock `lk` first (must wait until SLEEPING is set),
then `wq->lock` (queue is consistent), then `p->lock` crosses into
the scheduler. Each lock is released at the earliest point where its
protection is no longer needed — not mechanically in reverse.

### Spinlock vs mutex (naming)

| | Spinlock | Mutex |
|---|---|---|
| When contended | Busy-waits (spins) | Sleeps (yields CPU) |
| Hold duration | Very short (microseconds) | Can be long (disk I/O) |
| Interrupts | Disabled while held | Enabled while held |
| Implementation needs | Just atomic instruction | Needs sleep/wakeup underneath |

A mutex is built ON TOP of spinlock + sleep/wakeup — we can't build it
yet. That comes in Phase 7 (buffer cache needs locks held across I/O).

> **xv6 calls it "sleeplock."** Every other OS calls it a mutex. We'll
> call it `struct mutex` when we build it in Round 7-2.

---

## Part 4: Wait Queues and Sleep/Wakeup

### The problem with xv6's sleep channels

xv6's `sleep(chan, lock)` uses a `void *chan` (any address) as an
identifier. `wakeup(chan)` scans ALL processes looking for
`p->chan == chan`:

```c
// xv6: parent waits for child — sleep on own address
sleep(p, &wait_lock);
// Child exits — wake parent by its address
wakeup(p->parent);
```

The trick: every kernel object has a unique address, so any `&something`
is a valid channel. No registration needed.

> **The beauty of the void* trick:** using a physical address as an
> opaque identifier is genuinely clever. No allocation, no registration,
> no bookkeeping, guaranteed unique. If you only have a handful of
> processes (xv6's limit: 64), it's perfect. Real OSes moved to wait
> queues for scale, not because the idea is wrong — just because O(n)
> scan doesn't fly at O(thousands) of processes.

**Why we don't use this:** it scans every process on every wakeup.
We built `list.h` and `kmalloc` specifically to avoid O(n) scans.
Using a linear scan for the most fundamental synchronization primitive
contradicts our design philosophy.

### Wait queues: targeted wakeup via lists

Instead of scanning all processes, each **event** owns a list of its
waiters:

```c
struct wait_queue {
    struct spinlock lock;
    struct list_head head;
};
```

A process sleeping on an event links itself into that event's wait
queue via `wait_link` in `struct proc`. Wakeup only touches processes
on that specific queue — O(waiters) instead of O(all processes).

**API:**

```c
void wq_init(struct wait_queue *wq, const char *name);
void wq_sleep(struct wait_queue *wq, struct spinlock *lk);
int  wq_wake_one(struct wait_queue *wq);
void wq_wake_all(struct wait_queue *wq);
```

`wq_wake_one` prevents the **thundering herd** problem: waking all
waiters when only one can proceed means the rest immediately sleep again.

### Where wait queues live

Each waitable event embeds its own queue:

```c
struct proc {
    ...
    struct wait_queue child_wq;   // this proc sleeps here when calling wait()
    struct list_head wait_link;   // node for sleeping on any wait queue
};

// Future (Phase 8):
struct pipe {
    struct wait_queue rd_wq;      // readers wait here
    struct wait_queue wr_wq;      // writers wait here
};
```

> **What's actually on `child_wq`?** Not the children — they're tracked
> by the `children`/`sibling` list. The `child_wq` holds **the parent
> itself** (via its `wait_link` node) when it's sleeping in `wait()`.
> When a child exits, it does `wq_wake_one(&parent->child_wq)` to
> wake the parent. So `child_wq` is typically empty (parent isn't
> waiting) or has exactly one entry (the parent sleeping). In Phase 9
> with multiple threads, the queue might have more than one entry.

### The lost wakeup problem

The fundamental problem: a process must "check condition, then sleep if
false" — but what if the condition becomes true in the gap between
checking and sleeping?

**Attempt 1: No lock at all**

```
Consumer                          Producer
────────                          ────────
checks: buffer empty? YES
                                  writes data to buffer
                                  wq_wake_one() → queue empty, does nothing!
state = SLEEPING
list_add (on queue)
sched() → SLEEPING FOREVER
```

The producer slipped in between check and sleep. The wakeup found
nobody — and it's gone. The consumer will only recover if the producer
happens to write more data and calls `wq_wake_one` again. If the
producer is done, the consumer is stuck permanently.

**Attempt 2: Lock protects the condition check, but released before sleep**

```
Consumer                          Producer
────────                          ────────
spin_lock(lk)
checks: buffer empty? YES
spin_unlock(lk)  ← released!
                                  spin_lock(lk) ← succeeds
                                  writes data
                                  wq_wake_one() → queue EMPTY, does nothing!
                                  spin_unlock(lk)
state = SLEEPING
list_add (on queue)
sched() → SLEEPING FOREVER
```

Still broken. The lock prevented races during the check, but releasing
it before we're on the queue re-opens the same window.

**Attempt 3 (correct): Release lock AFTER we're on the queue and marked SLEEPING**

```
Consumer (in wq_sleep):              Producer:
────────                             ────────
holds lk
  spin_lock(&wq->lock)
  spin_lock(&p->lock)
  list_add (on queue)
  state = SLEEPING
  spin_unlock(lk)  ← NOW safe
                                      spin_lock(lk) ← unblocks
                                      writes data
                                      wq_wake_one() → finds us on queue → RUNNABLE ✓
```

By the time the producer can act (it needs `lk`), we're already safely
on the queue. Wakeup always finds us.

**What if the producer acts after we release `lk` but before sched()?**

```
Consumer:                            Producer:
────────                             ────────
  list_add (on queue)
  state = SLEEPING
  spin_unlock(lk)
                                      spin_lock(lk)
                                      writes data
                                      wq_wake_one():
                                        spin_lock(&wq->lock) ← OK
                                        spin_lock(&p->lock) ← BLOCKS (sleeper holds it!)
  spin_unlock(&wq->lock)
  sched() → swtch
  ─── scheduler releases p->lock ───
                                        spin_lock(&p->lock) ← now succeeds
                                        state = RUNNABLE, add to run queue
                                        spin_unlock(&p->lock)
```

Still correct — but the timing is more constrained than it first appears.
The waker **cannot** change state to RUNNABLE until after the sleeper has
passed through `sched()` → `swtch` and the scheduler has released
`p->lock`. The lost wakeup is prevented because the process is already
on the queue and marked SLEEPING before `lk` was released — the waker
will find it and wake it, just slightly later than the diagram suggests.

### The canonical pattern

The correct usage always follows this structure:

```c
// Consumer (sleeper):
spin_lock_irqsave(&condition_lock, &flags);
while (!condition_is_true()) {
    wq_sleep(&event_wq, &condition_lock);
    // wq_sleep releases condition_lock, sleeps, re-acquires it
}
// Condition is true — proceed
spin_unlock_irqrestore(&condition_lock, flags);
```

```c
// Producer (waker):
spin_lock_irqsave(&condition_lock, &flags);
make_condition_true();
wq_wake_one(&event_wq);
spin_unlock_irqrestore(&condition_lock, flags);
```

The `while` loop (not `if`) is essential:
1. **Spurious wakeup**: another consumer might have consumed the event
2. **Re-check after wake**: condition might have become false again

This pattern is universal — `pthread_cond_wait`, Java's `wait()`, Go's
`sync.Cond`. The abstraction is called a **condition variable**; our
wait queue IS one.

### `wq_sleep` implementation

```c
void
wq_sleep(struct wait_queue *wq, struct spinlock *lk) {
    struct proc *p = this_proc();

    spin_lock(&wq->lock);     // protect the queue
    spin_lock(&p->lock);      // protect our state

    list_add_tail(&p->wait_link, &wq->head);
    p->state = PROC_SLEEPING;

    spin_unlock(lk);          // NOW release condition lock — safe
    spin_unlock(&wq->lock);

    sched();                  // switch away (p->lock released by scheduler)

    spin_lock(lk);            // re-acquire condition lock before returning
}
```

The three locks serve different purposes:

| Lock | Protects | Acquired | Released |
|------|----------|----------|---------|
| `lk` (condition lock) | The condition (e.g., "any zombie?") | By caller, before wq_sleep | Inside wq_sleep, after SLEEPING is set |
| `wq->lock` | The linked list of waiters | Inside wq_sleep | Inside wq_sleep, after list_add |
| `p->lock` | Process state (RUNNING→SLEEPING) | Inside wq_sleep | By scheduler, after swtch (golden rule) |

> **Who removes `wait_link` from the queue?** The waker does — `wq_wake_one`
> acquires `wq->lock`, calls `list_del(&p->wait_link)` to remove the process
> from the queue, changes state to RUNNABLE, adds to run queue, then releases
> locks. By the time `wq_sleep` returns, the process is already off the queue.
> In `kill()`, the same applies: `kill` must remove the process from its wait
> queue (under `wq->lock`) before marking it RUNNABLE. This requires `kill` to
> know which queue the process is on — a pointer stored in `struct proc` or
> derivable from the `wait_link` node's list membership.

### Cross-boundary contracts: a kernel design pattern

`wq_sleep` is not a self-contained function. It has **preconditions and
postconditions that modify the caller's state**:

- Precondition: caller holds `lk` (acquired via `irqsave` — interrupts off)
- Side effect: `wq_sleep` releases `lk` (at a precise moment)
- Side effect: `wq_sleep` re-acquires `lk` before returning (plain
  `spin_lock`, not `irqsave` — interrupts are still off from the
  caller's original `irqsave`; the caller's `flags` variable remains
  valid on its stack and will eventually restore interrupts)

Why re-acquire? The caller re-checks the condition in a `while` loop
after waking — that check requires `lk`. By re-acquiring internally,
the caller's lock state is the same before and after the call. This is
the same pattern as POSIX `pthread_cond_wait(&cond, &mutex)` — the
standard user-space equivalent. (POSIX — Portable Operating System
Interface — is the IEEE standard defining the Unix API.)

In application programming, this would be terrible API design — hidden
side effects on caller state. In kernel code, it's necessary because
correctness (no lost wakeup) depends on the exact ordering of operations
that span caller and callee. The function is designed around *how its
caller must use it*.

This pattern appears throughout the kernel:

| Function | Contract |
|---|---|
| `wq_sleep(wq, lk)` | "Caller holds `lk`; I release it, sleep, re-acquire it" |
| `sched()` | "Caller holds `p->lock`; scheduler releases it on the other side" |
| `spin_lock_irqsave(lk, &flags)` | "I save YOUR interrupt state into YOUR variable" |

In application code, functions are black boxes — call them, get a result,
no shared state. In kernel code, functions are **cooperative partners**
with the caller: they share locks, modify each other's state, and their
correctness depends on the contract being honored from both sides. This
is another consequence of locks being advisory — the protocol exists
only in the programmer's discipline.

---

## Part 5: The Golden Rule of Process State Locking

### p->lock across swtch

`sched()` (the internal function that does `swtch` to the scheduler)
requires `p->lock` to be held. The lock is **held across swtch** —
acquired on one side, released on the other:

```c
// In scheduler():
for (;;) {
    spin_unlock(&prev->lock);       // release process that just yielded
    struct proc *p = pick_next();
    spin_lock(&p->lock);            // acquire next process's lock
    p->state = PROC_RUNNING;
    swtch(&c->scheduler, &p->context);
}

// In yield() or sleep():
spin_lock(&p->lock);
p->state = PROC_RUNNABLE;  // or SLEEPING
swtch(&p->context, &c->scheduler);
// scheduler acquired our lock before switching to us
spin_unlock(&p->lock);
```

### The weirdness: releasing someone else's lock

This is the most unusual thing about OS locking. The scheduler
releases a lock **it didn't acquire**:

```
Process A:                          Scheduler:
  spin_lock(&A->lock)  ← A acquires
  A->state = RUNNABLE
  swtch(A, scheduler)
  ─── A stops ───                   ─── resumes ───
                                    spin_unlock(&A->lock)  ← scheduler releases!
```

This is legal because a spinlock doesn't track **who** holds it — it's
just an integer (0 or 1). Anyone can set it back to 0. After `swtch`,
the scheduler runs on its own stack (the boot stack). Process A is
frozen — it will never touch its own lock until scheduled again. The
lock's job is done (protect the state transition), so whoever runs next
can release it.

This violates the normal rule ("same function acquires and releases")
but is correct because `swtch` is the one place where execution context
**transfers between two entities**. The lock protects the handoff itself.

> **Contrast with Rust:** Rust's `MutexGuard` enforces that only the
> holder can drop (release) the lock — the compiler rejects code that
> tries to release someone else's lock. In C, there's no such check.
> Our spinlock is a gentleman's agreement backed by `amoswap`. The
> same advisory principle that lets you skip locks entirely (and corrupt
> state) also lets you release locks you don't own (and enable the
> cross-swtch pattern). With great power comes great responsibility.

### Why this cross-boundary locking is necessary

Without it, on multi-hart:

```
Hart 0:                              Hart 1:
  spin_lock(&A->lock)
  A->state = RUNNABLE
  spin_unlock(&A->lock)  ← too early!
                                      scheduler1: sees A is RUNNABLE
                                      spin_lock(&A->lock) ← succeeds!
                                      A->state = RUNNING
                                      swtch(scheduler1, &A->context)
                                      ← A runs on Hart 1 with STALE context
  swtch(&A->context, scheduler0)
  ← saves A's regs into A->context
  ← but A is already running on Hart 1!
  ← two harts, same process, same kstack — total corruption
```

### The golden rule

All the complexity reduces to one rule:

> **`p->lock` must be held from the moment you change `p->state` until
> the corresponding `swtch` completes on the other side.**

This single rule prevents every scheduling race:

| Transition | Who acquires | Who releases | What it prevents |
|---|---|---|---|
| yield (RUNNING→RUNNABLE) | yield | scheduler | Another CPU running A before context saved |
| sleep (RUNNING→SLEEPING) | sleep | scheduler | Wakeup + run before fully asleep |
| wakeup (SLEEPING→RUNNABLE) | wakeup | wakeup (no swtch) | Another CPU running A before it's on run queue |
| scheduler (RUNNABLE→RUNNING) | scheduler | the process (after resume) | Two schedulers picking same process |

The pattern is always:
1. Acquire `p->lock`
2. Change `p->state`
3. `swtch` (lock crosses the boundary)
4. The other side releases `p->lock`

If you follow this one rule, you cannot get the multi-hart races.

---

## Part 6: exit() — Process Termination

### What exit does

A process calls `exit(status)` when it's done. This is the only way a
process terminates (even `kill` just causes the victim to call exit).

```c
void
exit(int status) {
    struct proc *p = this_proc();
    unsigned long flags;

    if (p->pid <= 1)
        panic("idle or init exiting");

    spin_lock_irqsave(&wait_lock, &flags);
    /* Reparent children to init */
    while (!list_empty(&p->children)) {
        struct proc *child = list_first_entry(&p->children, struct proc, sibling);
        child->parent = init_proc;
        list_del(&child->sibling);
        list_add_tail(&child->sibling, &init_proc->children);
    }
    wq_wake_one(&p->parent->child_wq);   // wake parent

    spin_lock(&p->lock);                  // inner lock (interrupts already off)
    p->exit_status = status;
    p->state = PROC_ZOMBIE;
    spin_unlock(&wait_lock);              // release wait_lock but keep interrupts off
                                          // (we still hold p->lock — can't restore yet)

    sched();          // never returns (p->lock released by scheduler)
    panic("zombie exit");
}
```

Note: we use `spin_unlock` (not `spin_unlock_irqrestore`) for `wait_lock`
here because we still hold `p->lock` — restoring interrupts would risk a
timer firing and attempting to yield while `p->lock` is held (deadlock).
Interrupts remain off through `sched()`. Since exit never returns, the
original `flags` is never used to restore — it's effectively discarded.

### Why we can't free ourselves

```c
// BROKEN:
kfree(p->kstack);    // we're EXECUTING ON this stack right now!
kmfree(p);           // we just freed the struct containing our context!
sched();             // p->context is in freed memory!
```

The process needs its own kstack to call `sched()` → `swtch()`. It needs
the proc struct to hold the context that swtch saves into. The parent
frees both in `wait()` — running on its own stack.

### Reparenting orphans

Reparenting is inlined in `exit()` (the while-loop above). It drains
`p->children` one by one, moving each child to `init_proc->children`.
Done under `wait_lock` because it modifies the same parent/children
pointers that `wait()` traverses.

Lock ordering in exit: `wait_lock` → `p->lock`. ✓

---

## Part 7: wait() — Reaping Children

### Three patterns of parent-child interaction

| Pattern | Parent calls wait()? | Who reaps? | Example |
|---------|---------------------|------------|---------|
| **Synchronize** | Yes, immediately | Parent | Shell waiting for a command |
| **Fire and forget** | No | Init (via reparenting) | Web server spawning handlers |
| **Garbage collector** | Yes, in a loop forever | Init itself | Init's entire job |

The "fire and forget" pattern requires the parent to set up the child
(it has the context — which socket, which request), but doesn't need the
result. Reparenting to init is the automatic safety net.

### What wait does

```c
int
wait(int *status) {
    struct proc *p = this_proc();
    unsigned long flags;

    spin_lock_irqsave(&wait_lock, &flags);
    for (;;) {
        struct proc *child;
        int found_child = 0;
        list_for_each_entry(child, &p->children, sibling) {
            found_child = 1;
            spin_lock(&child->lock);
            if (child->state == PROC_ZOMBIE) {
                int pid = child->pid;
                if (status)
                    *status = child->exit_status;
                spin_unlock(&child->lock);
                free_proc(child);
                spin_unlock_irqrestore(&wait_lock, flags);
                return pid;
            }
            spin_unlock(&child->lock);
        }

        if (!found_child) {
            spin_unlock_irqrestore(&wait_lock, flags);
            return -1;
        }

        // Children exist but none zombie — sleep
        wq_sleep(&p->child_wq, &wait_lock);
    }
}
```

### free_proc — final cleanup

```c
static void
free_proc(struct proc *p) {
    list_del(&p->sibling);     // parent's children list
    list_del(&p->all_list);    // global all-procs list
    list_del(&p->pid_link);    // PID hash table
    kfree((void *)p->kstack);
    kmfree(p);
}
```

Safe because: the parent is executing (on its own stack), the zombie
will never run again, and we hold `wait_lock`.

### Why two locks (wait_lock + p->lock)?

If children list were protected by `p->lock`:
- wait() holds `parent->lock`, wants `child->lock` (to check state)
- exit() holds `child->lock`, wants `parent->lock` (to reparent)
- **Deadlock**: circular dependency

The solution: `wait_lock` protects tree structure (parent/children
pointers), `p->lock` protects per-process state. They never conflict
because `wait_lock` is always acquired first.

---

## Part 8: kill() — Deferred Termination

### No OS has true "force kill"

Even `SIGKILL` (kill -9) doesn't rip a process off the CPU
mid-instruction. The kernel sets a flag; the process dies at a
well-defined point:

| Mechanism | How "forceful" | Still waits for... |
|---|---|---|
| Our `kill()` | Cooperative | Next trap return |
| Unix `SIGTERM` (15) | Cooperative | Process's signal handler (can ignore) |
| Unix `SIGKILL` (9) | Mandatory but deferred | Next kernel→user transition |
| Linux "D" state (uninterruptible sleep) | Nothing works | Hardware I/O completion |
| Power off | Truly immediate | Nothing |

Linux also has processes stuck in `TASK_UNINTERRUPTIBLE` sleep ("D" in
`ps`) — immune to SIGKILL until hardware I/O completes. (Linux later
added `TASK_KILLABLE` to reduce these situations.)

**Killing a process is always deferred.** The question is only how long.

### Implementation

```c
int
kill(int pid) {
    int bucket = hash_int(pid) & (HT_SIZE(PID_HASH_BITS) - 1);
    struct proc *p;
    int found = 0;

    list_for_each_entry(p, &pid_table[bucket], pid_link) {
        if (p->pid == pid) { found = 1; break; }
    }
    if (!found) return -1;

    unsigned long irq;
    spin_lock_irqsave(&p->lock, &irq);
    p->killed = 1;
    if (p->state == PROC_SLEEPING) {
        list_del(&p->wait_link);   /* TODO(Phase 9): needs wq->lock */
        p->state = PROC_RUNNABLE;
        run_queue_add(p);
    }
    spin_unlock_irqrestore(&p->lock, irq);
    return 0;
}
```

### Where `killed` is checked

At safe points — after handling a trap, and in interruptible sleep loops:

```c
// In kernel_trap_ret:
void kernel_trap_ret(void) {
    struct cpu *c = this_cpu();
    if (c->proc && c->proc->killed)
        exit(-1);
    if (c->need_resched && c->proc) {
        c->need_resched = 0;
        yield();
    }
}

// In sleep loops:
while (!condition) {
    if (p->killed) { spin_unlock(&lk); exit(-1); }
    wq_sleep(&wq, &lk);
}
```

### Why wake a sleeping process?

If we just set the flag and the process is sleeping, it might sleep
forever (waiting for pipe data that will never come). Marking it
RUNNABLE ensures it gets scheduled, checks `killed`, and exits cleanly.

Maximum latency: depends on scheduling. The process must be scheduled
and run through `kernel_trap_ret` to notice the flag. With round-robin
and N runnable processes, worst case is up to N × timeslice (every
other process gets a full turn first).

---

## Part 9: Updated Data Structures

### `struct proc`

```c
struct proc {
    struct spinlock lock;       // protects: state, killed, exit_status

    /* --- Identity --- */
    int pid;
    char name[PROC_NAME_LEN];
    enum proc_state state;

    /* --- Scheduling --- */
    struct list_head all_list;
    struct list_head run_list;
    struct list_head pid_link;

    /* --- Family --- */
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;
    struct wait_queue child_wq; // this proc sleeps here when calling wait()

    /* --- Execution state --- */
    struct context context;
    uint64 kstack;

    /* --- Address space (Phase 6) --- */
    pagetable_t pagetable;
    struct trapframe *trapframe;
    uint64 sz;

    /* --- Lifecycle --- */
    int killed;
    int exit_status;
    struct list_head wait_link; // for sleeping on a wait queue
};
```

### `struct cpu`

```c
struct cpu {
    struct proc *proc;         // currently running process, or NULL
    struct context scheduler;  // scheduler's saved context
    int need_resched;          // timer set this; kernel_trap_ret checks it
};
```

No `intr_depth` or `intena` — our irqsave pattern stores interrupt
state in local stack variables rather than per-CPU fields.

### Global state

```c
struct spinlock wait_lock;      // protects process tree relationships
struct spinlock run_queue_lock; // protects run queue
struct spinlock pid_lock;       // protects PID allocation + hash table
struct proc *init_proc;          // pointer to PID 1
```

---

## Part 10: How Much Do Locks Cost?

### Instruction-level cost (uncontended)

When no one else wants the lock, the cost is just the instructions:

| Operation | Cycles (approximate) |
|---|---|
| `amoswap.w.aq` (atomic swap) | ~5 |
| `fence rw,rw` (pipeline drain) | ~5 |
| CSR read + write (irqsave) | ~4 |
| **Total per lock/unlock pair** | **~15-20** |

One context switch touches ~4 lock pairs (run_queue_lock, p->lock on
each side) = ~80 cycles. With a 10ms timeslice at 10MHz = 100,000
cycles per timeslice: **0.08% overhead**. Invisible.

### Contention model (multi-hart)

When multiple harts want the same lock, the real cost is **spinning** —
waiting for someone else to release. A simple model:

**Parameters:**
- N = number of harts
- C = critical section length (cycles the lock is held)
- T = interval between lock attempts on each hart (cycles)
- f = C/T = fraction of time each hart holds the lock

**Probability another hart holds it when you try:** (N-1) × f

**Expected spin per attempt:** (N-1) × (C/T) × (C/2)

(You arrive uniformly during someone else's C-cycle hold, so average
spin is C/2.)

### Three scenarios

**Scenario 1: Process state change (our run_queue_lock)**
- N = 4 harts, C = 50 cycles, T = 100,000 (once per 10ms timeslice)
- P(contention) = 3 × 50/100,000 = 0.15%
- Expected spin = 0.0015 × 25 = **0.04 cycles**
- Verdict: **invisible** — lock accessed too infrequently to contend

**Scenario 2: Console output (every 1ms)**
- N = 4 harts, C = 500 cycles (50 chars), T = 10,000
- P(contention) = 3 × 500/10,000 = 15%
- Expected spin = 0.15 × 250 = **37 cycles per attempt**
- Overhead on 500-cycle critical section: 7.5%
- Verdict: **noticeable** — might want per-CPU output buffers

**Scenario 3: Global allocator under load**
- N = 8 harts, C = 100 cycles, T = 2,000 (frequent allocation)
- P(contention) = 7 × 100/2,000 = 35%
- Expected spin = 0.35 × 50 = **17.5 cycles per attempt**
- Effective throughput: ~3 out of 8 harts doing useful work
- Verdict: **broken** — need per-CPU freelists

### The pattern: when locks hurt and how to fix

| Problem | Symptom | Fix |
|---|---|---|
| Long critical section | High C, other CPUs spin for ages | Shorten (buffer then flush, split work) |
| Many CPUs, same lock | High N × f, frequent contention | Fine-grained: per-CPU or per-object locks |
| High-frequency access | Low T, many attempts per timeslice | Reduce lock scope, batch operations |

For our single-hart Phase 5: contention is zero (nobody to contend
with). The overhead is purely ~20 cycles per lock/unlock — rounding
error. Contention becomes real in Phase 9, which is exactly why that
phase focuses on fine-grained locking, per-CPU data structures, and
lock scalability analysis.

---

## Part 11: bobchouOS vs xv6

| Aspect | xv6 | bobchouOS |
|--------|-----|-----------|
| Sleep primitive | `void *chan` — scan all procs | `struct wait_queue` — targeted list |
| Wakeup cost | O(NPROC) = O(64) | O(waiters on queue) |
| Interrupt save | `push_off`/`pop_off` (per-CPU noff) | `irqsave` pattern (stack-local flags) |
| Mutex | "sleeplock" | `struct mutex` (Phase 7) |
| Process free | Release to fixed array (state → UNUSED) | `kmfree(p)` — back to heap |
| Init proc | Loads `/init` from disk | Kernel thread (user init in Phase 6) |
| kill mechanism | Same: set flag, wake if sleeping | Same |
| fork | User syscall (copies trapframe + user pages) | Phase 6 only — kernel uses `proc_create_kernel` |

---

## Quick Reference

### Process states

| State | Meaning | In run queue? | On a wait queue? |
|-------|---------|---------------|-----------------|
| `PROC_RUNNABLE` | Ready, waiting for CPU | Yes | No |
| `PROC_RUNNING` | Executing on a hart | No | No |
| `PROC_SLEEPING` | Waiting for an event | No | Yes |
| `PROC_ZOMBIE` | Dead, waiting for parent to reap | No | No |

### Rules

**1. Spinlock fundamentals:**
- Spinlocks are advisory — hardware does not enforce. Programmer's responsibility.
- A spinlock doesn't track who holds it (just 0 or 1). Anyone can release it.
- `amoswap` provides atomicity (hardware-enforced). `fence` provides ordering. Both are needed.

**2. Interrupt discipline:**
- Disable interrupts before acquiring any spinlock (prevents self-deadlock from interrupt handlers).
- Use `spin_lock_irqsave` for outermost lock; plain `spin_lock` for inner locks when interrupts are provably off.
- Each `irqsave`/`irqrestore` pair needs its own `flags` variable (cannot reuse).

**3. Lock ordering (acquisition order, never violated):**
```
wait_lock → wq->lock → p->lock
```
(`run_queue_lock` and `pid_lock` are self-contained in helpers — not in the ordering chain.)
Release order: reverse by default; semantics may override (releasing never causes deadlock, only acquisition order matters).

**4. The golden rule (scheduling):**
> `p->lock` must be held from the moment you change `p->state` until the corresponding `swtch` completes on the other side.

This enables the cross-swtch pattern: process acquires, scheduler releases (or vice versa).

**5. Sleep/wakeup:**
- Always check condition in a `while` loop (not `if`) — handles spurious wakeups.
- `wq_sleep` releases the condition lock only AFTER marking SLEEPING and joining the queue (solves lost wakeup).
- `wq_sleep` re-acquires the condition lock before returning (POSIX condvar pattern).
- Wakeups don't accumulate — a missed one is gone permanently.

**6. Process lifecycle:**
- A process cannot free itself (still on its own kstack). Parent must reap via `wait()`.
- Every process has a parent. Orphans are reparented to init.
- `kill()` just sets a flag — termination is always deferred to next safe point.
- Idle (PID 0) never sleeps. Init (PID 1) never exits.

### Key functions

| Function | Purpose |
|----------|---------|
| `spin_lock_irqsave(lk, &flags)` / `spin_unlock_irqrestore(lk, flags)` | Acquire/release, save/restore interrupts (outermost lock) |
| `spin_lock(lk)` / `spin_unlock(lk)` | Acquire/release, no interrupt change (inner lock, interrupts already off) |
| `wq_sleep(wq, lk)` | Sleep on queue, atomically releasing lk |
| `wq_wake_one(wq)` / `wq_wake_all(wq)` | Wake waiters |
| `exit(status)` | Terminate: ZOMBIE, reparent, wake parent |
| `wait(&status)` | Reap zombie child, sleep if none |
| `kill(pid)` | Set killed flag, wake if sleeping |
| `yield()` | Give up CPU (voluntary or timer-forced) |
| `sched()` | Internal: swtch to scheduler (must hold p->lock) |

### Lifecycle — state diagram with lock annotations

```
                        proc_create_kernel [A]
                                |
                                v
    +-------------------->[RUNNABLE]<---------------------+
    |                           |                         |
    |                    scheduler [B]               wq_wake_one [F]
    |                           |                    kill [G]
    |                           v                         |
    |                       [RUNNING]                     |
    |                      /    |    \                    |
    |              yield [C] exit [E] wq_sleep [D]        |
    |                  |        |          |              |
    +------------------+     [ZOMBIE]   [SLEEPING]--------+
                                |
                           wait [H]
                                |
                             (freed)
```

### Lifecycle — detailed lock trace

Each transition shows: function, locks acquired (irq = irqsave, plain = interrupts already off), and who releases.

```
═══════════════════════════════════════════════════════════════════════

[A] CREATION (proc_create_kernel):
    pid_lock [plain, self-contained in alloc_pid]
    run_queue_lock [plain, self-contained in run_queue_add]
    → state = RUNNABLE, on run queue

═══════════════════════════════════════════════════════════════════════

[B] RUNNABLE → RUNNING (scheduler):
    acquire: run_queue_lock [plain]        → pick_next, release run_queue_lock
    acquire: p->lock [plain]               → set RUNNING, swtch to process
    release: p->lock                       → by PROCESS after swtch
                                             (kthread_start for new, or
                                              spin_unlock_irqrestore in yield,
                                              or spin_lock(lk) path in wq_sleep)

═══════════════════════════════════════════════════════════════════════

[C] RUNNING → RUNNABLE (yield):
    acquire: p->lock [irq]                 → set RUNNABLE
    run_queue_add: run_queue_lock [plain, self-contained]
    sched(): swtch to scheduler            → p->lock crosses boundary
    release: p->lock                       → by SCHEDULER after swtch returns
    restore: irq flags via irqrestore

═══════════════════════════════════════════════════════════════════════

[D] RUNNING → SLEEPING (wq_sleep):
    (caller already holds lk [irq] — e.g. wait_lock)
    acquire: wq->lock [plain]
    acquire: p->lock [plain]               → set SLEEPING, add to wq
    release: lk (condition lock)           → lost wakeup solved
    release: wq->lock                      → queue consistent
    sched(): swtch to scheduler            → p->lock crosses boundary
    release: p->lock                       → by SCHEDULER after swtch returns
    acquire: lk [plain]                    → re-acquire condition lock (POSIX)

═══════════════════════════════════════════════════════════════════════

[F] SLEEPING → RUNNABLE (wq_wake_one):
    acquire: wq->lock [irq]                → remove from queue (list_del)
    acquire: p->lock [plain]               → set RUNNABLE
    release: p->lock
    run_queue_add: run_queue_lock [plain, self-contained]
    release: wq->lock via irqrestore

═══════════════════════════════════════════════════════════════════════

[E] RUNNING → ZOMBIE (exit):
    acquire: wait_lock [irq, dead flags]   → reparent children, wake parent
      wq_wake_one internally: wq->lock [irq], p->lock [plain]
    acquire: p->lock [plain]               → set ZOMBIE, set exit_status
    release: wait_lock [plain — still hold p->lock]
    sched(): swtch to scheduler            → p->lock crosses boundary
    release: p->lock                       → by SCHEDULER (never returns)

═══════════════════════════════════════════════════════════════════════

[H] ZOMBIE → freed (wait):
    acquire: wait_lock [irq]               → scan children list
    acquire: child->lock [plain]           → check state
    release: child->lock                   → if ZOMBIE, proceed to free
    free_proc: list_del (sibling, all_list, pid_link), kfree, kmfree
    release: wait_lock via irqrestore
    return pid

    If no zombie: wq_sleep(&child_wq, &wait_lock)
      → follows SLEEPING path above (lk = wait_lock)

═══════════════════════════════════════════════════════════════════════

[G] SLEEPING → RUNNABLE via kill():
    acquire: p->lock [irq]                 → set killed=1
    list_del(&p->wait_link)                → remove from wait queue (TODO: wq->lock)
    set RUNNABLE
    run_queue_add: run_queue_lock [plain, self-contained]
    release: p->lock via irqrestore

    Process later checks killed in kernel_trap_ret or sleep loop → exit(-1)

═══════════════════════════════════════════════════════════════════════
```

**Legend:**
- `[irq]` = acquired via `spin_lock_irqsave` (saves + disables interrupts)
- `[plain]` = acquired via `spin_lock` (interrupts already off)
- `[irq, dead flags]` = irqsave used only to disable interrupts; flags never restored (function doesn't return)
- `self-contained` = acquired and released within a helper; not in the ordering chain
