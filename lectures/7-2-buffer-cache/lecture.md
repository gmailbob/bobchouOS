# Lecture 7-2: Buffer Cache

> **Where we are**
>
> Round 7-1 gave us a single primitive: `virtio_blk_rw(struct buf *b)`
> moves one block between disk and a `struct buf` you hand it, sleeping
> until the device signals completion. That's the whole disk interface —
> raw, synchronous, one transfer per call.
>
> But nothing above it can use a raw transfer primitive safely. Two
> processes that both touch block 42 would each allocate their own
> `struct buf`, issue their own reads, and end up with two independent
> in-memory copies — and if both write, the disk gets whichever finishes
> last. There is no caching (every read hits the platter), no
> coordination (two writers race), and no notion of "the" block 42.
>
> ```
> Round 7-6:  open/read/write/close  (user-facing syscalls)
> Round 7-5:  directories + paths    (naming)
> Round 7-4:  inodes + bitmap        (on-disk structure)
> Round 7-3:  write-ahead log        (crash safety)
> Round 7-2:  buffer cache           (in-memory cache of blocks)  ← you are here
> Round 7-1:  virtio-blk driver      (talk to the hardware)
> ```
>
> The buffer cache is the layer that turns "transfer a block" into
> "borrow *the* block." Everything above it — the log, inodes,
> directories, files — reads and writes the disk *only* through this
> cache, and relies on its central guarantee: **at most one copy of each
> disk block lives in memory, and at most one process operates on it at a
> time.**
>
> **What you will understand after this lecture:**
>
> - Why a buffer cache merges two jobs — caching and synchronization — into one structure
> - The `bread`/`bwrite`/`brelse` API and why it has exactly this shape
> - Why a spinlock cannot guard a block held across disk I/O, and what a *sleep-lock* is
> - How to build a general sleep-lock (kernel mutex) on top of our wait queue
> - The `bget` lookup-or-recycle algorithm, and the two-lock dance at its heart
> - LRU eviction over an intrusive list, and the `valid` flag's role
> - How two processes racing for the same block stay correct
> - Where `bpin`/`bunpin` fit, and why the log (7-3) will need them

> **xv6 book coverage:**
> Ch 8 §8.1–8.2 ("Code: Buffer cache"). Our approach follows xv6
> closely — same `bget`/`bread`/`bwrite`/`brelse` API, same two-loop
> recycle, same LRU. The differences: our sleep-lock is built as a
> standalone general primitive on top of the **wait queue** from Lecture
> 5-2 (xv6 builds its sleeplock on a raw `chan` sleep/wakeup), and our
> LRU uses the intrusive `list.h` from Lecture 5-0 rather than xv6's
> open-coded `prev`/`next` pointers.

---

## Part 1: Why a Buffer Cache Exists

### Two problems, one structure

Stack the file system on top of the bare driver and two problems appear
immediately. They look unrelated, but the buffer cache is the answer to
both — and the key insight of this lecture is *why solving them together
is simpler than solving them apart*.

**Problem 1 — caching (performance).** A disk read is glacial compared
to memory. Even our emulated virtio device involves a descriptor chain,
a doorbell write, an interrupt, and a context switch. Real hardware adds
milliseconds. But disk access is enormously repetitive: the file
system reads the same inode block every time it walks a path, the same
bitmap block every time it allocates, the same directory block every
time it does a lookup. If those blocks stay in RAM after the first read,
nearly all of that I/O disappears. A cache hit is a memory copy; a
cache miss is a disk transfer. We want hits.

**Problem 2 — synchronization (correctness).** Suppose two processes
both want to append to the same directory. Each must read the directory
block, modify it, and write it back. If they each keep a private copy,
their updates clobber each other — classic lost update. Even a single
block must have a *single* in-memory home that all processes share, with
mutual exclusion so that "read, modify, write" sequences don't
interleave.

Here is the insight. Both problems are solved by the same rule:

> **Invariant:** for each disk block, there is **at most one copy** in
> memory, and that copy is used by **at most one process at a time.**

The "at most one copy" half is exactly what makes caching correct
(everyone sees the same data) *and* what makes synchronization possible
(there's a single object to lock). Caching and locking aren't two
features bolted together — they're two consequences of one invariant.
That's why xv6 (and we) put them in one structure, the buffer cache,
rather than a cache layer and a separate lock layer.

### Where it sits

```
        ┌─────────────────────────────────────────┐
        │  file system: inodes, directories, log  │   (7-3 … 7-6)
        └─────────────────────────────────────────┘
                  │ bread / bwrite / brelse
                  ▼
        ┌─────────────────────────────────────────┐
        │           buffer cache (7-2)            │   ← this lecture
        │  • caches recently-used blocks in RAM   │
        │  • one locked buf per (dev, blockno)    │
        └─────────────────────────────────────────┘
                  │ virtio_blk_rw  (on a miss only)
                  ▼
        ┌─────────────────────────────────────────┐
        │         virtio-blk driver (7-1)         │
        └─────────────────────────────────────────┘
```

The file system never calls `virtio_blk_rw` directly again. It calls
`bread` and gets back a locked, valid buffer; it calls `bwrite` to push
changes; it calls `brelse` to let go. The driver is invoked by the cache
*only on a miss*. That indirection is the whole point.

---

## Part 2: The API

The buffer cache exposes four core operations plus two pin helpers. The
shape is worth memorizing, because the entire file system above is
written against it.

```c
struct buf *bread(uint32 dev, uint32 blockno);  // get a locked buf with valid data
void        bwrite(struct buf *b);              // flush a locked buf to disk now
void        brelse(struct buf *b);              // release the lock, drop the ref
void        bpin(struct buf *b);                // bump refcnt (survives brelse)
void        bunpin(struct buf *b);              // drop the pinned ref
```

**`bread(dev, blockno)` → locked buf.** Returns *the* buffer for that
block, holding its sleep-lock, with `data[]` guaranteed to hold the
block's current contents. On a cache hit it's a memory operation; on a
miss it calls `virtio_blk_rw` to fill the buffer first. The caller owns
the buffer until it calls `brelse`.

> **One device, for now.** The `dev` argument is forward-looking. We have
> exactly one virtio-blk device, so `dev` is effectively always the same
> value and `(dev, blockno)` is really just `blockno` today — the 7-1
> smoke test never even sets `b->dev`. We carry `dev` from the start
> because it's the natural cache key (a real system caches blocks from
> many devices in one cache) and because threading it in later would
> touch every call site. Until multiple devices exist, mentally read
> `bread(dev, blockno)` as "read block `blockno`."

**`bwrite(b)`.** Writes a locked buffer's `data[]` back to disk,
synchronously. The caller must hold the buffer's lock (it came from
`bread`). Note *who* calls this: in 7-2, file-system-less, only tests
do. In 7-3 the write-ahead log will interpose — file system code will
call `log_write(b)`, and the log calls `bwrite` at commit time. So
`bwrite` is the *primitive*, not the interface the FS ultimately uses.
We build the primitive now and wrap it later.

**`brelse(b)`.** ("b-release.") Releases the sleep-lock so another
process can acquire the buffer, and drops this caller's reference. It
also moves the buffer to the most-recently-used position so the LRU
eviction policy keeps it around. Every `bread` must be paired with a
`brelse`, like malloc/free.

**`bpin(b)` / `bunpin(b)`.** A reference-count bump that *survives*
`brelse`. Normally a buffer with no active `bread` holder is eligible
for eviction; a pinned buffer is not, even when unlocked. We include
these now even though nothing uses them in 7-2, because 7-3's log needs
exactly this: a block that has been written into the on-disk log but not
yet installed at its home location must not be evicted and re-read from
its (stale) home. Adding them now keeps `bio.c` closed in 7-3 — the same
"don't reopen the file next round" reasoning that put the virtio request
header inside `struct buf` back in 7-1.

### The lifecycle

```
   bread(dev, blockno)
        │
        ├── hit:  find buf, acquire its sleep-lock ────┐
        │                                              │
        └── miss: recycle a buf, virtio_blk_rw, ───────┤
                  (lock already held)                  │
                                                       ▼
                                          ┌───────────────────────┐
                                          │  caller owns the buf  │
                                          │  reads/writes data[]  │
                                          │  optionally bwrite()  │
                                          └───────────────────────┘
                                                       │
                                                   brelse(b)
                                                       │
                                            unlock, drop ref, MRU
```

---

## Part 3: The Sleep-Lock — a Lock You Can Hold Across I/O

### Why the spinlock we already have won't do

Look again at the lifecycle. Between `bread` and `brelse`, the buffer is
locked — and on a miss, `bread` calls `virtio_blk_rw`, which *sleeps*
until the disk interrupt fires. So the buffer's lock is held across a
disk transfer: potentially milliseconds, during which the holder is not
running.

Our spinlock from Lecture 5-2 cannot do this. Two independent reasons,
either one fatal:

1. **A spinlock's irqsave variant disables interrupts**
   (`spin_lock_irqsave` calls `intr_off`, saving the prior state to
   restore on unlock). But the event we're waiting for — the disk
   completion — *is* an interrupt. Hold such a spinlock across the
   transfer and you've masked the very interrupt that would wake you.
   Deadlock.

2. **You may not sleep holding a spinlock.** The whole contract of a
   spinlock is that critical sections are short and the holder stays on
   the CPU — other CPUs *busy-wait* (spin) for it. If the holder slept,
   the spinners would burn cycles for milliseconds, and on a single hart
   the system would simply hang (the holder is asleep, nothing else can
   run to wake it).

> **Is this enforced?** In our kernel, no — it's a convention, and
> `sched()` doesn't check it. xv6 *does* enforce it, with **two**
> complementary checks in `sched()`: an *identity* check
> (`holding(&p->lock)` — you hold the proc lock) **and** a *count* check
> (`noff == 1` — and nothing else). It needs both because a count alone
> can't tell *which* lock is held — you could release `p->lock`, grab
> some condition lock instead, and still pass `noff == 1`; the identity
> check is what closes that gap. Together they assert the exact rule:
> "you hold `p->lock`, and only `p->lock`." Linux enforces it even more
> strictly — `might_sleep()` fires at the start of every blocking call,
> plus lockdep validates lock *identity and ordering* at runtime — but
> through dedicated machinery, not the irqsave flags themselves.
> Enforcement is orthogonal to the flag style: we adopted the modern
> save/restore `irqsave` flags but simply didn't add a checker.

So the buffer cache forces a new requirement that no existing lock
meets: **a lock that may be held for a long time, during which the
holder sleeps, and other contenders sleep too instead of spinning.**
That is a *sleep-lock* — what the rest of the world calls a mutex.

> **Sleep-lock = mutex.** "Sleep-lock" names the *behavior* (you sleep
> while waiting); "mutex" names the *contract* (mutual exclusion, one
> owner). They're the same object. We keep the name `sleeplock` to pair
> with `spinlock` — the two names sit on the axis that actually matters
> in a kernel: **do you spin or sleep while you wait?** A spinlock is
> also "mutual exclusion, one owner," so calling this one "mutex"
> wouldn't distinguish them. xv6 makes the same naming choice.

### The two locks, side by side

| | spinlock | sleeplock |
|---|---|---|
| Wait by | spinning (busy-wait) | sleeping (yield the CPU) |
| Interrupts while held | disabled (irqsave) | enabled |
| May hold across I/O / sleep? | **no** | **yes** |
| Critical section length | nanoseconds | milliseconds OK |
| Built on | `amoswap` atomic | spinlock + wait queue |
| Linux analogue | `spinlock_t` | `struct mutex` |
| Used for | run queue, kalloc, the cache index | a `struct buf`, later inodes (7-5) |

The "Built on" row is the key structural fact: **a
sleep-lock is not a competitor to the spinlock — it's built out of
one.** A short spinlock protects the sleep-lock's own internal state;
the wait queue is where contenders sleep. Let's build it, then step back
to see the layering that fact implies.

### Building it

We have both ingredients already: the spinlock (5-2) and the wait queue
(5-2). The sleep-lock is a thin composition:

```c
struct sleeplock {
    int locked;             // is the lock held?
    struct wait_queue wq;   // contenders sleep here (Lecture 5-2)
    struct spinlock lk;     // protects `locked` — held only for nanoseconds
    int pid;                // holder's pid (debugging / sleep_holding)
};
```

> **Two "locked"s, two durations — why we need both `locked` and `lk`.**
> A natural objection: `struct spinlock lk` already *has* a `locked` bit
> of its own — why a *separate* `int locked`? Because they track different
> things over different timescales:
>
> - **`sl->locked`** is the **ownership** flag. It is `1` for the *entire*
>   span a holder owns the lock — from `sleep_lock` to `sleep_unlock`,
>   across the disk I/O, across the caller modifying `data[]`. That can be
>   *milliseconds*. This is the lock's actual meaning.
> - **`sl->lk`** is a short internal spinlock held only for the *few
>   nanoseconds* it takes to read-and-update `sl->locked` and touch the
>   wait queue. It is **released the instant `sleep_lock` returns** — so
>   for almost the whole time the sleep-lock is held, `sl->lk` is *free*
>   and `sl->locked == 1`. They have opposite values for nearly the entire
>   lifetime; they cannot be the same field.
>
> They *must* be separate because you may not hold a spinlock across I/O
> (Part 3). If we reused `lk`'s own bit as the ownership flag — i.e. held
> `lk` from acquire to release — we'd be spinning CPUs and masking
> interrupts for the whole millisecond-long transfer, exactly what the
> sleep-lock exists to avoid. So `locked` carries the long-lived
> ownership; `lk` is just the nanosecond-scale guard that makes updating
> it atomic. This is the universal shape of *every* blocking lock: a
> **condition** representing the real state, plus a **short spinlock**
> guarding that condition's bookkeeping. You've seen it already —
> `wait_queue` is `head` (state) guarded by `wq->lock`; the 7-1 driver is
> `b->disk` (state) guarded by `disk.lock`. Linux's `struct mutex` is the
> same: an owner field plus an internal `wait_lock`.

With the two fields clear, the code is short. `sleep_lock` is the
canonical condition-loop from Lecture 5-2:

```c
void
sleep_lock(struct sleeplock *sl) {
    spin_lock(&sl->lk);
    while (sl->locked)            // condition: someone else holds it
        wq_sleep(&sl->wq, &sl->lk);  // sleep, atomically releasing sl->lk
    sl->locked = 1;
    sl->pid = this_proc()->pid;
    spin_unlock(&sl->lk);
}
```

That `while`-loop-sleeping-on-a-condition is the lost-wakeup-safe
pattern you built in 5-2: re-check after every wake, never assume the
wakeup means the condition is now true. The sleep-lock is just that
pattern, packaged as a reusable object. `sleep_unlock` clears `locked`
and wakes one waiter:

```c
void
sleep_unlock(struct sleeplock *sl) {
    spin_lock(&sl->lk);
    sl->locked = 0;
    sl->pid = 0;
    wq_wake_one(&sl->wq);         // hand off to one sleeper (if any)
    spin_unlock(&sl->lk);
}
```

> **Why `wq_wake_one`, not `wq_wake_all`?** Only one contender can take
> the lock; waking all of them just to have all-but-one re-sleep is a
> "thundering herd." `wq_wake_one` wakes a single waiter — the one that
> will actually win. (Contrast the driver in 7-1, which `wq_wake_all`s:
> there, *every* sleeper is waiting on a *different* buf's completion, so
> all must re-check.)

`sleep_holding` peeks at `locked` for debug assertions — the sleep-lock
analogue of `spin_holding`. It's why we store `pid`: a panic can say
*which* process held a lock, and assertions can check "do I hold this?"

This sleep-lock is a **general kernel mutex**, not a buffer-specific
gadget. `struct buf` will embed one; in Round 7-5 `struct inode` embeds
the same type for `ilock`/`iunlock`; the log header in 7-3 uses one too.
Building it standalone now means none of those rounds touch lock code —
they just declare a `struct sleeplock` member and call
`sleep_lock`/`sleep_unlock`.

### The lock hierarchy — what's fundamental, and why

Now that we've seen a sleep-lock built *out of* a spinlock, the layering
generalizes into something worth carrying forward. Among locks, **the
spinlock is the most fundamental** — everything else in the kernel is
built on top of it:

```
   sleeplock      ── built on ──►  spinlock + wait_queue
   wait_queue     ── built on ──►  spinlock (+ scheduler)
   spinlock       ── built on ──►  amoswap (hardware atomic) + intr_off
   amoswap        ── IS ─────────  a single RISC-V instruction
```

Every higher-level lock bottoms out in the spinlock: the sleeplock's
internal `lk` is a spinlock, the wait queue's `lock` is a spinlock, the
scheduler's run-queue lock is a spinlock. Nothing is built without it,
and it depends on no other lock. *That* is the precise sense of "most
fundamental" — it's the base of the dependency graph among locks.

But notice the spinlock itself isn't self-sufficient. It rests on two
things that are **not** locks — the true bedrock beneath all locking:

1. **A hardware atomic instruction** — `amoswap.w` (atomic swap). The
   CPU guarantees "read the old value and write 1" happens indivisibly,
   even with other harts hammering the same word. You *cannot* build a
   correct lock in software without some hardware atomic (swap,
   compare-and-swap, or load-reserved/store-conditional). This is a
   result, not an implementation quirk: pure loads and stores can do
   mutual exclusion in theory (Peterson's algorithm) but not on real
   out-of-order multi-core hardware. The atomic is the absolute floor.

2. **Interrupt control** — `intr_off`. A spinlock guards against *other
   harts*; on a *single* hart it must also guard against an interrupt
   handler grabbing the same lock (e.g. the virtio ISR touching a
   structure the interrupted code had locked → self-deadlock). So the
   irqsave variant disables interrupts for the critical section.

So a sharper statement than "spinlock is most fundamental" is: **the
spinlock is the thinnest possible wrapper that turns two hardware
capabilities — an atomic instruction and interrupt masking — into the
abstraction "mutual exclusion."** Everything else (wait_queue,
sleeplock, and through them the buffer cache, inode locks, pipes) is
built on top.

And the layering can only go this way — it's forced, not a style choice.
Ask why the sleeplock can't be the foundation instead:

- A **sleeplock**, to put a waiter to sleep, must touch the scheduler's
  run queue and a wait queue — shared structures that themselves need
  mutual exclusion. What protects *them*? A spinlock. So the sleeplock
  *depends on* the spinlock; it cannot be more fundamental.
- A **spinlock**, to wait, just spins. It needs no scheduler, no process
  abstraction, nothing — it works the instant a CPU is executing, even
  before the scheduler exists. With no dependencies on the rest of the
  kernel, it can sit at the bottom.

The general principle: **the lock that waits by doing nothing (spinning)
can be primitive; the lock that waits by sleeping cannot, because
"sleeping" is itself a kernel service that needs locking.** You always
build the sleeping lock out of the spinning lock, never the reverse.

### Three kinds of sleep — don't confuse them

We just built the sleep-lock on the wait queue, so it's tempting to
over-generalize — to think all sleeping is one mechanism, or that we
could fold one form into another. We can't, and seeing why sharpens what
each is *for*.

Our kernel has **three** distinct ways a process sleeps, and they differ
on the dimension that actually defines a sleep mechanism: **who decides
when you wake, and on what key.**

| | event sleep (`wait_queue`) | mutex (`sleeplock`) | timed sleep (`tsleep`) |
|---|---|---|---|
| Wake condition | another thread signals the queue | holder releases the lock | wall-clock time reaches a deadline |
| Who wakes you | a `wq_wake_one`/`all` caller | a `sleep_unlock` caller | the timer interrupt (`wake_expired_sleepers`) |
| Queue order | FIFO (insertion) | FIFO | sorted by `wake_time` |
| Wakeup picks | front / all | one | all entries past `now`, stops at first future one |
| Key | the queue object | the lock | a per-proc `uint64 wake_time` |

The first two are **event-driven**: some other thread runs code that
wakes you (`wq_wake_*`, or `sleep_unlock`). The third is
**deadline-driven**: *nobody calls wake on your behalf* — the timer ISR
periodically scans a list ordered by time and wakes whoever's deadline
has passed. That sort-by-time and wake-a-prefix discipline (the `break`
at the first future entry in `wake_expired_sleepers`) is what no event
queue has.

So could we collapse them? No, and the failures are instructive:

- **"Just use a sleeplock for `sys_sleep`."** A sleeplock answers one
  question — "is someone holding this?" — and `sleep_unlock` wakes a
  waiter when a holder leaves. But a timed sleep has *no holder and no
  releaser*; there's nothing to acquire or release. You'd have to invent
  a fake lock that the timer "releases" at the right moment — and to know
  that moment you'd still need `wake_time` and the sorted list. You'd
  have rebuilt `tsleep` with a meaningless lock bolted on. Timed sleep
  isn't mutual exclusion, so the mutex abstraction doesn't fit.

- **"Then make `tsleep` a plain `wait_queue`."** Closer — `tsleep` really
  is "sleep on a queue, get woken later." But a `wait_queue` is unordered
  and `wq_wake_*` wakes the front or *all*; it can't wake "only those
  past their deadline." Every tick you'd `wq_wake_all`, and nearly every
  woken proc would re-check the clock and go straight back to sleep — the
  **thundering herd** the sorted list exists to prevent. The sort key and
  deadline-arming would have to live *somewhere*, and that somewhere is
  exactly the `tsleep_list` + `wake_time` machinery. Folding it in would
  burden the common event-sleep path (fork/wait, the buffer cache,
  sleep-locks) with timer baggage it never uses.

The honest takeaway: `tsleep` *is* the right abstraction for its
category — a **timer/timeout queue** (ordered by deadline, woken by the
clock), the third primitive in the standard kernel kit alongside the
event queue and the mutex. We just haven't promoted it to a named
reusable type, because it has exactly one user (`sys_sleep`) — the same
"one caller, no second on the roadmap" judgment that keeps us from
building a semaphore. If it ever *does* get abstracted, the interesting
version isn't "merge it with the sleeplock"; it's replacing its O(n)
sorted-list insert with an ordered tree (Linux's timer wheel / timerqueue
is an rbtree for exactly this reason).

> **The shared skeleton, the divergent policy.** All three sit on the
> same two primitives — a spinlock to guard the queue, and a way to park
> a sleeping proc. What differs is *wakeup policy*: signalled, released,
> or timed. This is the recurring kernel pattern — a small mechanism
> (park/unpark under a lock) wrapped in different policies. Don't mistake
> the shared mechanism for sameness of purpose.

### Aside: the userspace story (futexes, a preview)

Everything above is the *kernel* picture, where the spinlock is the
fundamental lock. Userspace is different, and it's worth one preview to
see why — full treatment is Phase 9-3.

A user-space mutex (e.g. a pthread mutex) cannot use a kernel spinlock:
user code can't disable interrupts, can't assume it stays scheduled, and
shouldn't burn a whole timeslice spinning. The modern answer is the
**futex** ("fast userspace mutex"), which splits the lock in two:

```
   USERSPACE:  a plain int in shared memory + an atomic CAS on it
                       │
                       │  (only when the CAS shows contention)
                       ▼
   KERNEL:      futex() syscall ──► wait queue (keyed by the int's address)
                                    protected by ... a SPINLOCK
```

- **Uncontended fast path** — lock/unlock is *just an atomic
  compare-and-swap* on the int, entirely in userspace. No syscall, no
  kernel, no lock at all. This is the whole point: before futexes, every
  mutex operation trapped into the kernel and took a lock; the futex
  makes the common (uncontended) case never leave userspace.
- **Contended slow path** — only when the CAS fails (someone else holds
  it) does the thread make the `futex(FUTEX_WAIT)` syscall. And *that*
  path bottoms out on a kernel **spinlock** — the kernel keeps a wait
  queue hashed by the user address, protected by a spinlock, structurally
  identical to our `wait_queue` + `spinlock` + sleeplock pattern.

So a futex does **not** escape the spinlock — it escapes the *kernel*, on
the uncontended path. Once a thread actually has to block, it's right
back on a kernel spinlock-protected wait queue, exactly like our
sleeplock. The lesson reinforces the hierarchy above rather than breaking
it: the atomic instruction is the floor everywhere; the kernel spinlock
is the foundation of all *blocking* synchronization, kernel or user; the
futex's contribution is skipping both whenever the lock is uncontended.

---

## Part 4: `struct buf` Grows Up

In 7-1 we deliberately defined `struct buf` with just the fields the
driver needs, and left a comment promising the cache fields would be
*purely additive*. Here they are. Nothing existing changes:

```c
struct buf {
    uint32 dev;                 // device number          ──┐ identity:
    uint32 blockno;             // logical block number     │ (dev, blockno)
                                //                        ──┘ is the key
    uint8  data[BSIZE];         // the block's contents (4096 bytes)

    // --- virtio driver fields (Round 7-1) ---
    struct virtio_blk_req req;  // request header (type + sector)
    uint8  status;              // device result: 0=ok, 1=err, 2=unsupp
    int    disk;                // 1 = in-flight (device owns buf)

    // --- buffer-cache fields (Round 7-2, additive) ---
    int    valid;               // has data[] been read from disk?
    int    refcnt;              // how many bread holders + pins?
    struct sleeplock lock;      // held bread → brelse (the "operating" lock)
    struct list_head lru_link;  // links this buf into the LRU list
};
```

Four new fields, each with a distinct job:

- **`valid`** — does `data[]` actually hold the block's contents? A
  freshly recycled buffer has `valid == 0` (its `data[]` is some *other*
  block's stale bytes); `bread` must read from disk before returning it.
  Once read, `valid == 1` and future `bread`s skip the disk. This is the
  flag that makes a cache hit a cache hit.

- **`refcnt`** — how many callers currently "have" this buffer:
  outstanding `bread`s that haven't `brelse`d, plus any `bpin`s. A buffer
  with `refcnt > 0` is in use and **must not be evicted**. `refcnt == 0`
  means it's a cache entry nobody's using right now — keep it for its
  caching value, but it's a candidate for recycling under pressure.

- **`lock`** — the sleep-lock from Part 3, held from `bread` to
  `brelse`. This is the "one process operates on the block at a time"
  half of our invariant.

- **`lru_link`** — the intrusive list node (Lecture 5-0) that threads
  this buffer into the cache's LRU ordering. Recall intrusive lists embed
  the link *in* the element, so no allocation is needed to enqueue.

### Two locks, two jobs — don't confuse them

The buffer cache has **two distinct locks**, and a recurring source of
confusion is conflating them:

| Lock | Type | Protects | Held for |
|---|---|---|---|
| `bcache.lock` | spinlock (one, global) | the *set* of buffers: the LRU list, `refcnt` fields, the hit/miss decision | nanoseconds — just the lookup |
| `b->lock` | sleeplock (one per buf) | the *contents* of one buffer: `data[]`, `valid` | the whole bread→brelse span, across I/O |

The spinlock guards the *index* — "which buffers exist and who's using
them." The per-buffer sleeplock guards the *data* — "I'm operating on
this block, hands off." You hold the spinlock for a few instructions to
find or recycle a buffer, then *release it* and acquire the sleeplock to
use the buffer. The next part is exactly that dance.

---

## Part 5: `bget` — the Heart of the Cache

Everything interesting happens in `bget(dev, blockno)`, the internal
routine `bread` is built on. Its job: return the buffer for `(dev,
blockno)`, locked, creating/recycling one if it isn't cached. It has the
classic xv6 **two-loop** structure.

```c
static struct buf *
bget(uint32 dev, uint32 blockno) {
    spin_lock(&bcache.lock);

    // Loop 1: is the block already cached?
    list_for_each_entry(b, &bcache.lru, lru_link) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;                 // claim a reference
            spin_unlock(&bcache.lock);   // release the index lock...
            sleep_lock(&b->lock);      // ...THEN take the data lock
            return b;
        }
    }

    // Loop 2: not cached — recycle the least-recently-used unused buf.
    list_for_each_entry_reverse(b, &bcache.lru, lru_link) {
        if (b->refcnt == 0) {            // unused → evictable
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;                // contents are stale → force a read
            b->refcnt = 1;
            spin_unlock(&bcache.lock);
            sleep_lock(&b->lock);
            return b;
        }
    }

    panic("bget: no buffers");           // all NBUF in use at once
}
```

Four subtle points, each worth its own beat.

### Subtlety 1 — release the spinlock *before* the sleeplock

In both loops we do `spin_unlock(&bcache.lock)` and *then*
`sleep_lock(&b->lock)`. Never the reverse. Why? Because
`sleep_lock` may sleep — and you must never sleep holding a spinlock
(Part 3). If we acquired the sleeplock while holding `bcache.lock`, a
contended buffer would put us to sleep with the global index lock held,
freezing the entire cache for everyone. So: bump `refcnt` under the
spinlock (that's enough to reserve the buffer — see Subtlety 2), drop
the spinlock, *then* contend for the data lock.

### Subtlety 2 — `refcnt++` under the spinlock is the reservation

The instant we do `b->refcnt++` and release `bcache.lock`, the buffer is
*reserved* even though we don't hold its sleeplock yet. Eviction (Loop
2) only recycles buffers with `refcnt == 0`, and `refcnt` is read and
written only under `bcache.lock`. So between our `refcnt++` and our
`sleep_lock`, no other process can steal this buffer for a different
block — they'll see `refcnt > 0` and skip it. We might *wait* for the
sleeplock (another process is using this same block), but the buffer's
identity can't change underneath us. The spinlock protects the
reservation; the sleeplock protects the use.

### Subtlety 3 — Loop 2 scans backward (LRU)

`bcache.lru` is ordered most-recently-used at the front, least at the
back (Part 6 maintains this). Loop 2 iterates *in reverse* — from the
back — so it evicts the **least** recently used unused buffer first.
That's the LRU policy: the block nobody's touched for the longest is the
one least likely to be needed again, so it's the cheapest to throw away.

Note Loop 2 sets `valid = 0`. The recycled buffer still holds some other
block's bytes in `data[]`; marking it invalid forces `bread` to read the
real block from disk before handing it back. That's the next part.

### Subtlety 4 — the final `panic` is an assertion, not lazy handling

That `panic("bget: no buffers")` looks like a cop-out — a real kernel
surely shouldn't crash when a cache fills up. But it's defensible here,
and seeing *why* peels back three distinct problems people conflate.

**It's an assertion of an invariant.** `bget` only fails if *all* `NBUF`
buffers have `refcnt > 0` at the same instant — every buffer actively
held between someone's `bread` and `brelse`. The FS is designed so no
single operation holds more than a small fixed number of buffers at
once, so as long as `NBUF ≥ (max held per op) × (concurrent ops)`, this
is *unreachable*. NBUF=30 is sized above that bound. So if it ever fires,
it means a bug — a forgotten `brelse` (leak) or an op holding too many —
and panicking on a broken invariant is correct: you don't want to limp
along corrupting the file system.

**The "obvious" fix — sleep and retry — is actually wrong.** It's
tempting to replace the panic with "sleep on a wait queue until a buffer
frees, then retry," waking it from `brelse` when a `refcnt` hits 0. But
if all holders are themselves blocked in `bget` waiting for *one more*
buffer (inode block held, now needs a bitmap block), no one reaches
`brelse` — **circular wait, deadlock.** The retry trades a loud crash for
a silent hang unless paired with a deadlock-avoidance discipline (acquire
all of an op's buffers atomically, or back off and release everything).
The static bound above is what lets us skip both the retry *and* the
deadlock proof.

**Even a correct retry wouldn't stop a *deliberate* exhauster.** A retry
answers "pool empty → now what?" It never answers "*who* emptied it, and
were they allowed to?" `refcnt` is anonymous — it counts *that* N holders
exist, not *which* processes hold them — so there's nobody to throttle or
kill. Defending against a process that intentionally exhausts buffers
needs a different layer entirely: **per-process resource accounting**
(attribute each held buffer to an owner), **quotas** (cap how many one
process may hold), and **forced reclaim/kill** when a process exceeds its
share — the job of rlimits / cgroups / the OOM-killer in Linux. That's a
cross-cutting subsystem, not a `bget` tweak, and out of scope for a
single-hart cooperative kernel whose threat model has no malicious
processes yet.

The throughline: a **statically bounded** pool treats exhaustion as a bug
(assert/panic — us); a **dynamic** pool treats it as backpressure
(reclaim, block, retry — Linux's page cache); and *neither* is the same
as **defending against abuse**, which needs accounting and enforcement on
top. 7-2 correctly picks the first, for the threat model it actually has.

### `bread` and `bwrite`

With `bget` done, the public calls are tiny:

```c
struct buf *
bread(uint32 dev, uint32 blockno) {
    struct buf *b = bget(dev, blockno);   // locked, but maybe not valid
    if (!b->valid) {
        b->req.type = VIRTIO_BLK_T_IN;    // direction = read
        virtio_blk_rw(b);                 // miss: fetch from disk, sleeping
        b->valid = 1;                     // now data[] is real
    }
    return b;                             // hit or miss: valid + locked
}

void
bwrite(struct buf *b) {
    if (!sleep_holding(&b->lock))
        panic("bwrite: buf not locked");  // you must own it
    virtio_blk_rw(b);                     // (driver fills direction = write)
}
```

`bread`'s `if (!b->valid)` is the entire hit/miss distinction. A hit
(`valid == 1`, the common case) returns immediately — pure memory. A
miss reads from disk exactly once, then `valid` stays 1 for every future
`bread` of that block until the buffer is recycled. `bwrite` just asserts
ownership and pushes the bytes down.

> **The direction flag.** In 7-1, `virtio_blk_submit` hardcoded
> `write = 0` (the block-0 smoke test only read). The skeleton for 7-2
> threads a real read/write flag through so `bread` requests a read and
> `bwrite` requests a write. That's the one driver change this round — a
> small, anticipated one.

---

## Part 6: `brelse` and LRU Maintenance

`brelse` is `bread`'s mirror: release the data lock, drop the reference,
and update the LRU ordering.

```c
void
brelse(struct buf *b) {
    if (!sleep_holding(&b->lock))
        panic("brelse: buf not locked");

    sleep_unlock(&b->lock);               // let the next process in

    spin_lock(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0) {                 // nobody's using it now
        // move to the front: most-recently-used
        list_del(&b->lru_link);
        list_add(&b->lru_link, &bcache.lru);
    }
    spin_unlock(&bcache.lock);
}
```

Note the lock pairing is the reverse of `bget`: release the *sleeplock*
first (outside the spinlock — `sleep_unlock` is cheap but the rule is to
not nest), then take the *spinlock* to touch `refcnt` and the list.

The LRU update only happens when `refcnt` hits 0 — i.e., when the last
user lets go. At that moment the buffer becomes a pure cache entry, and
we move it to the **front** of `bcache.lru` to mark it most-recently
used. Since Loop 2 of `bget` evicts from the *back*, this front-insertion
is exactly what keeps hot blocks resident and lets cold blocks fall off
the tail.

```
   bcache.lru:    [MRU] ←───────────────────────────────→ [LRU]
                  front                                    back
                    ▲                                       │
                    │ brelse moves here                     │ bget Loop 2
                    │ (refcnt → 0)                          ▼ evicts from here
```

### Why LRU?

Eviction needs a policy: when the pool is full, *which* unused buffer do
we recycle? LRU is a bet on **temporal locality** — a block that was
accessed recently is more likely to be accessed again soon than one
nobody's touched for a while. File system workloads are heavily
repetitive: the same inode blocks, directory blocks, and bitmap blocks
get re-read over and over across many operations. So the
least-recently-used buffer — the one furthest in the past — is the
safest bet for "won't be needed again soon," and the cheapest to throw
away.

It's not perfect. A single sequential scan of many distinct blocks (e.g.
reading a large file linearly) pollutes the cache with blocks you'll
never revisit, pushing out hot blocks that *would* have been re-read.
But at NBUF=30 with a typical filesystem workload dominated by repeated
metadata access, simple LRU is a good cheap heuristic — and all it costs
is one `list_del` + `list_add` in `brelse`.

For comparison, other policies and when they'd matter:

| Policy | Evicts | Good at | Bad at |
|---|---|---|---|
| **LRU** (us, xv6) | oldest-untouched | repeated access to a working set | one-time sequential scans |
| FIFO | oldest-arrived | simple, no per-access bookkeeping | ignores access pattern entirely |
| LRU-K / 2-list (Linux) | "inactive" list; promoted to "active" only after K accesses | resisting scan pollution | more complex (two lists + promotion logic) |

Linux's page cache uses a two-list active/inactive approximation
(LRU-2-ish) precisely to resist scan pollution — a single `read()` of a
huge file doesn't flush the entire hot working set. We don't need that
sophistication at 30 buffers; LRU is the right match for the scale.

### `bpin` / `bunpin`

```c
void bpin(struct buf *b)   { spin_lock(&bcache.lock); b->refcnt++; spin_unlock(&bcache.lock); }
void bunpin(struct buf *b) { spin_lock(&bcache.lock); b->refcnt--; spin_unlock(&bcache.lock); }
```

They just adjust `refcnt` under the index lock — no sleeplock involved,
because pinning is about *eviction eligibility*, not *operating on the
data*. A pinned buffer (`refcnt > 0`) survives `brelse` and stays out of
Loop 2's reach. As noted in Part 2, 7-3's log is the consumer: a block
committed to the log but not yet installed at home is pinned so a recycle
can't drop it and force a stale re-read.

### `binit`

One-time setup at boot: initialize the spinlock, build the LRU list, and
init each buffer's sleeplock.

```c
void
binit(void) {
    spin_init(&bcache.lock, "bcache");
    INIT_LIST_HEAD(&bcache.lru);
    for (int i = 0; i < NBUF; i++) {
        struct buf *b = &bcache.buf[i];
        sleep_init(&b->lock, "buffer");
        list_add(&b->lru_link, &bcache.lru);   // all buffers start in the list
    }
}
```

The buffers live in a static array `bcache.buf[NBUF]` (NBUF = 30,
following xv6). All start threaded into the LRU list with `refcnt == 0`
and `valid == 0` — a pool of empty, recyclable slots.

> **Why a fixed pool, not "use all free RAM"?** Linux's page cache is
> dynamic — it grows to fill available memory, then *shrinks* under
> memory pressure. But a dynamic cache needs a whole subsystem to answer
> "memory is running low — which pages do I evict *now*?" That's the
> **page reclaimer**: memory-pressure detection, dirty-page writeback, a
> background kernel thread (`kswapd`) that frees pages before you run out,
> and integration with the page allocator so cache growth and process
> allocation compete fairly. Linux has thousands of lines for this; we
> have `kalloc` handing out fixed pages with no pressure signal, no
> background reclaimer, no concept of "the cache should shrink."
>
> At our scale — one device, a handful of concurrent FS operations,
> ~16 MB RAM — 30 fixed buffers (120 KiB) is big enough to hold the hot
> working set of metadata blocks, and small enough that it's a trivial
> fraction of RAM. The dynamic approach trades simplicity for a benefit
> (using more RAM for cache) that doesn't matter at our memory size and
> workload. If we ever wanted to go dynamic, the prerequisite is the page
> allocator gaining a "low memory" callback that tells the cache "give
> back N pages" — part of the same resource-management arc discussed in
> Subtlety 4, and a post-Phase-9 concern.

---

## Part 7: Concurrency — Walking the Races

The design's correctness rests on a few interleavings. Let's trace the
ones that matter.

### Race A — two processes want the same block

P1 and P2 both `bread(dev, 42)`, block 42 not yet cached.

```
P1: bcache.lock ──► Loop1 miss ──► Loop2 finds victim V, V.blockno=42,
                    V.refcnt=1, unlock bcache.lock ──► sleep_lock(V.lock) ✓
                    ──► reads block 42 from disk ──► using it
P2:                 bcache.lock ──► Loop1 HIT (V.dev/blockno match!),
                    V.refcnt=2, unlock ──► sleep_lock(V.lock) … sleeps
                    (P1 holds it)
P1: brelse ──► sleep_unlock(V.lock) ──► wakes P2
P2: sleep_lock returns ──► using the SAME buf V ✓
```

The outcome we wanted: one buffer, shared. P2's `bget` ran *after* P1
bumped `refcnt` and inserted block 42's identity into V (under the
spinlock), so P2's Loop 1 sees it as a hit. Both end up with the same
`struct buf`; the sleeplock serializes their *use* of it. And because P1
already set `valid = 1` after its disk read, P2's `bread` skips the disk
entirely — P2 gets a free cache hit off P1's work.

### Race B — could two processes recycle the same victim?

No. Loop 2 runs entirely under `bcache.lock`, and so does the
`refcnt++`/identity-write that claims the victim. Two processes can't
both be in Loop 2 simultaneously (the spinlock serializes them), so the
first to run claims victim V (sets `refcnt=1`); the second, when it runs,
sees `V.refcnt != 0` and scans past it to a different victim. The
spinlock makes "find a free buffer and claim it" atomic.

### Race C — the completion interrupt during a miss

During a miss, `bread` calls `virtio_blk_rw`, which sleeps on the
*driver's* wait queue (`disk.wq`), not the buffer's sleeplock. The
buffer's sleeplock stays held the whole time — correct, because we're
mid-operation on it. When the disk interrupt fires, `virtio_blk_intr`
clears `b->disk` and wakes us; we set `valid = 1` and return the
still-locked buffer. The two wait queues (driver completion vs. buffer
ownership) are independent and never confused.

### The lock-ordering rule

There's a strict ordering, and every path obeys it:

```
   bcache.lock   (spinlock, index)   ── acquire first, release before sleeping
        →
   b->lock       (sleeplock, data)   ── acquire second, may sleep
```

You take the spinlock, do the O(1)-ish index work, **release it**, then
take the sleeplock. You never hold both at once across a sleep — that's
the rule that keeps the whole cache from freezing, and it's the same
"don't sleep holding a spinlock" law from Part 3, applied structurally.

---

## Part 8: bobchouOS vs xv6, and a Look at Linux

With the design built and its correctness argued, let's step back and
compare our buffer cache against xv6 and Linux — what we share, where we
diverge, and why.

### bobchouOS vs xv6

| Aspect | xv6 | bobchouOS | Linux |
|---|---|---|---|
| API | `bread`/`bwrite`/`brelse`/`bpin`/`bunpin` | same | `find_get_page`/`mark_page_dirty`/`put_page` (page cache) |
| Lookup structure | two loops (linear scan of NBUF) | same | per-inode radix tree / xarray (O(log n)) |
| Eviction | LRU via hand-coded `prev`/`next` | LRU via intrusive `list.h` (5-0) | two-list active/inactive (LRU-2-ish, scan-resistant) |
| Lock (per-block) | `sleeplock` on `chan`-based sleep | `sleeplock` on `wait_queue` (5-2) | page lock (`PG_locked` bit + `wait_queue_head_t`) |
| Lock (index) | single spinlock | same | per-inode lock + RCU for readers |
| Cache unit | `struct buf` (fixed BSIZE block) | same | `struct page` (4 KiB page, variable per-file extent) |
| `struct buf` layout | driver req in side-array | driver req embedded (7-1) | `struct bio` + `struct buffer_head` (separate) |
| Pool size | 30 static bufs | 30 static bufs | dynamic — all free RAM is cache (reclaim under pressure) |
| Writeback | synchronous (`bwrite`) | same | asynchronous (pdflush/writeback threads) |
| Block size | 1024 (BSIZE) | 4096 (= page size) | typically 4096 (page-aligned) |

### A look at Linux (preview)

Linux solves the same problem — "cache disk blocks in RAM, one copy per
block, locked for exclusive use" — but at a completely different scale
(gigabytes of cache, millions of pages, dozens of CPUs). That scale
forces every design choice to be different, even though the *principle*
is identical. Here's how:

**What's cached.** We cache fixed 4 KiB blocks in a static 30-buffer
pool (see "Why a fixed pool?" in Part 6 for the rationale). Linux has no
fixed pool — **all free RAM is cache**. `struct page` objects hold file
data, the pool grows until memory fills, and the "page reclaim"
subsystem shrinks it under pressure.

**How to find a cached page.** We do a linear scan of 30 buffers — fine
at that size. Linux keeps a **per-file radix tree** (now called an
"xarray") that maps `(file, page-offset)` → `struct page *` in O(log n).
This means finding a cached page is fast even when millions of pages are
cached across thousands of files.

**Eviction.** We use simple LRU (see Part 6's "Why LRU?" for the
tradeoff). Linux uses a **two-list active/inactive** system that resists
scan pollution: a page must be accessed *twice* to earn a spot on the
"active" list, so a one-time sequential read can't flush the hot set.

**Writeback.** Our `bwrite` is synchronous — the calling process sleeps
until the disk confirms the write. Linux does **asynchronous writeback**:
dirty pages accumulate, and dedicated kernel threads (`writeback`
threads) flush them to disk in the background. The process that dirtied
the page doesn't wait; it just marks it dirty and moves on. The kernel
flushes later, batching many writes for efficiency.

**Where `struct buffer_head` fits.** Some older Linux filesystems still
think in terms of disk blocks (like our `struct buf`). For those, Linux
attaches a `struct buffer_head` to each page, describing which disk
blocks the page's bytes correspond to. Newer filesystems (ext4 in extent
mode, btrfs, XFS) skip `buffer_head` entirely and work directly with
pages and block-I/O requests (`struct bio`). So `buffer_head` is roughly
Linux's version of our `struct buf` — a legacy bridge between
block-oriented FS code and the page-oriented cache.

**The principle is the same.** Despite all these differences, the
invariant is identical to ours: one canonical in-memory copy of each
disk block, shared and locked. Our 30-buffer cache is that same idea
with the scale turned down so the structure is visible.

---

## Quick Reference

### Buffer Cache API

| Function | Signature | Purpose |
|---|---|---|
| `binit` | `void binit(void)` | One-time boot setup: lock, LRU list, per-buf sleeplocks |
| `bread` | `struct buf *bread(uint32 dev, uint32 blockno)` | Return *the* buf, locked + valid (disk read on miss) |
| `bwrite` | `void bwrite(struct buf *b)` | Flush a locked buf to disk now |
| `brelse` | `void brelse(struct buf *b)` | Release lock, drop ref, move to MRU |
| `bpin` | `void bpin(struct buf *b)` | Bump refcnt — survives brelse (for 7-3 log) |
| `bunpin` | `void bunpin(struct buf *b)` | Drop the pinned ref |
| `bget` (static) | `struct buf *bget(uint32 dev, uint32 blockno)` | Internal: lookup-or-recycle, returns locked buf |

### Sleep-Lock API

| Function | Signature | Purpose |
|---|---|---|
| `sleep_init` | `void sleep_init(struct sleeplock *sl, const char *name)` | Initialize |
| `sleep_lock` | `void sleep_lock(struct sleeplock *sl)` | Acquire; sleeps while held by another |
| `sleep_unlock` | `void sleep_unlock(struct sleeplock *sl)` | Release; wakes one waiter |
| `sleep_holding` | `int sleep_holding(struct sleeplock *sl)` | Debug: do I hold it? |

### struct buf (expanded in 7-2)

```c
struct buf {
    uint32 dev;                 // device number          ┐ key =
    uint32 blockno;             // logical block number   ┘ (dev, blockno)
    uint8  data[BSIZE];         // block contents (4096 bytes)
    struct virtio_blk_req req;  // driver: request header (7-1)
    uint8  status;              // driver: device result (7-1)
    int    disk;                // driver: 1 = in-flight  (7-1)
    int    valid;               // cache: data[] read from disk?
    int    refcnt;              // cache: # holders + pins (0 = evictable)
    struct sleeplock lock;      // cache: held bread → brelse
    struct list_head lru_link;  // cache: LRU list node (5-0)
};
```

### struct sleeplock

```c
struct sleeplock {
    int locked;             // held?
    struct wait_queue wq;   // sleepers (Lecture 5-2)
    struct spinlock lk;     // protects `locked` (held nanoseconds)
    int pid;                // holder pid (debug)
};
```

### The two locks — never confuse them

| Lock | Type | Scope | Protects | Held for |
|---|---|---|---|---|
| `bcache.lock` | spinlock | one, global | LRU list, all `refcnt`, hit/miss decision | nanoseconds (index work) |
| `b->lock` | sleeplock | per buffer | `data[]`, `valid` of one block | bread → brelse (across I/O) |

### Lock ordering

```
bcache.lock (spinlock)  →  b->lock (sleeplock)
   acquire first            acquire second
   release BEFORE sleeping   may sleep
```

### Key Invariants

| Invariant | Enforced by |
|---|---|
| At most one in-memory copy per (dev, blockno) | `bget` Loop 1 hit reuses the existing buf |
| At most one process operates on a buf at a time | per-buf sleeplock, held bread → brelse |
| A reserved buf can't be recycled out from under you | `refcnt++` under `bcache.lock` before unlock |
| Recycled buffers get re-read, not served stale | `valid = 0` on recycle, `bread` checks it |
| Never sleep holding the index lock | release `bcache.lock` before `sleep_lock` |
| Pinned blocks survive eviction | `refcnt > 0` keeps a buf out of Loop 2 |

### Constants

| Name | Value | Meaning |
|---|---|---|
| NBUF | 30 | Number of cache buffers (static array) |
| BSIZE | 4096 | Block size = page size (from 7-1) |
