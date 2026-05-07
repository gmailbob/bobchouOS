# Lecture 5-0: Intrusive Containers (list & hashtable)

> **Where we are:** We have a fully working memory subsystem — buddy allocator,
> slab allocator, virtual memory. We're about to build the process subsystem
> (Phase 5), which needs processes to live in multiple data structures
> simultaneously: a global list, a run queue, a parent's children list, and a
> PID hash table. Before we can build `struct proc`, we need the container
> primitives it depends on.
>
> **What this lecture covers:** Intrusive doubly-linked lists and intrusive hash
> tables — the two fundamental container patterns used by essentially every OS
> kernel. We'll understand *why* they work the way they do, *how* the C
> machinery (offsetof, container_of) makes them type-generic, and *what
> invariants* must hold.
>
> **xv6 book coverage:** xv6 doesn't use intrusive containers — it uses a flat
> `proc[NPROC]` array and scans it for everything. We're departing from xv6
> here to match how real kernels (Linux, FreeBSD, Windows) organize their data.

---

## Part 1: Why Intrusive?

### The problem

A process in our kernel needs to be in multiple containers at once:

| Container | Purpose |
|-----------|---------|
| `all_procs` list | Enumeration (ps, kill -9 all) |
| `run_queue` list | Scheduler picks next RUNNABLE process |
| `children` list | Parent's wait() scans only its children |
| `pid_table` hash | O(1) lookup by PID (kill, waitpid) |

In a language like Java, you'd put the same object reference into four different
collections and the garbage collector handles the rest. In C, there is no
standard container library, no garbage collector, and no runtime type
information.

### Two approaches to building containers in C

**Approach A — External (non-intrusive):**

```c
// A generic list node that wraps a void* to your data:
struct list_node {
    void *data;              // C has no generics — void* is "any type"; caller casts on retrieval
    struct list_node *next;
    struct list_node *prev;
};
```

To insert a process into the run queue, you `kmalloc` a `list_node`, set
`data = proc`, and link it in. To remove, you unlink and `kmfree` the node.

Problems:
1. **Extra allocation per insertion** — in an interrupt handler or with the
   scheduler lock held, you cannot afford to call kmalloc (it might sleep, or
   you just don't want that latency).

   > **Note:** Linux's `kmalloc` takes allocation flags — `GFP_KERNEL` allows
   > sleeping (the allocator can reclaim memory by evicting pages, swapping,
   > etc.), while `GFP_ATOMIC` forbids sleeping (returns NULL if nothing is
   > immediately available). Sleeping under a spinlock or inside an interrupt
   > handler deadlocks the system. Our bobchouOS `kmalloc` never sleeps — it
   > returns immediately or panics — but the principle remains: even without
   > sleeping, allocation is variable-latency work you want to avoid in
   > performance-critical paths.
2. **Separate allocation per membership** — if a process is in 4 containers,
   that's 4 separate `kmalloc` calls to create wrapper nodes, and 4 `kmfree`
   calls on removal. (The intrusive approach also stores link pointers — but
   they're pre-allocated inside the struct in one shot, with no per-insertion
   allocation and no cleanup bookkeeping.)
3. **Indirection** — given a `list_node`, you dereference `.data` to reach the
   process. One extra pointer hop, one cache miss.
4. **Removal requires finding the node** — if you have a `struct proc *p` and
   want to remove it from the run queue, you must first *find* which `list_node`
   wraps it. That's O(n) unless you also maintain a back-pointer from proc to
   each wrapping node — at which point you've reinvented the intrusive approach
   with extra steps.

**Approach B — Intrusive:**

```c
// The link node lives INSIDE your struct:
struct proc {
    int pid;
    struct list_head all_list;   // link for global list
    struct list_head run_list;   // link for run queue
    struct list_head sibling;    // link for parent's children list
    struct list_head pid_link;   // link for PID hash table bucket chain
};
```

No external allocation. The "container plumbing" is embedded. One struct in four
containers costs exactly 8 pointers total (4 `list_head` fields × 2 pointers
each), all embedded in the struct itself — no separate nodes. Removal is O(1) given just `p` — you know exactly where `p->run_list` is
and can unlink it directly.

### Who uses what

| Project | Pattern | Container library |
|---------|---------|-------------------|
| Linux kernel | Intrusive | `<linux/list.h>`, `<linux/hashtable.h>` |
| FreeBSD kernel | Intrusive | `<sys/queue.h>` (TAILQ, LIST, SLIST) |
| Windows kernel | Intrusive | `LIST_ENTRY`, `RTL_HASH_TABLE` |
| GLib (GNOME userspace) | External | `GList`, `GHashTable` (void* data) |
| C++ STL | External | `std::list<T>`, `std::unordered_map<K,V>` |

Every major OS kernel uses intrusive containers. Userspace libraries tend toward
external containers because allocation is low-consequence — you can call malloc
freely from any context, the kernel handles reclaim/swap behind the scenes, and
failure just kills your process, not the whole system. In a kernel, allocation
may be impossible (interrupt context, spinlock held), failure is catastrophic,
and every call site must reason about "what if this fails." Intrusive containers
sidestep the problem entirely — no allocation needed for list operations.

---

## Part 2: The C Machinery — `offsetof` and `container_of`

The fundamental challenge of intrusive containers: given a pointer to an embedded
`list_head`, how do you get back to the enclosing struct?

```
                    struct proc (at address 0x1000)
                    ┌──────────────────────────────────────┐
0x1000              │ pid          (4 bytes)               │
0x1004              │ padding      (4 bytes)               │
0x1008              │ all_list     ← list_head (16 bytes)  │
0x1018              │ run_list     ← list_head (16 bytes)  │
0x1028              │ ...                                  │
                    └──────────────────────────────────────┘
```

If you have a pointer to `run_list` (address `0x1018`) and you know it's the
`run_list` field of a `struct proc`, then the enclosing proc must be at:

```
proc_address = run_list_address - offset_of_run_list_within_proc
             = 0x1018 - 0x18
             = 0x1000
```

That's exactly what `container_of` does.

### `offsetof(type, member)`

Returns the byte offset of `member` within `type`:

```c
#define offsetof(type, member) ((uint64)&((type *)0)->member)
```

The trick: cast 0 to a pointer to `type`, access the member, take its address.
Since the base is 0, the address *is* the offset. This is a compile-time
constant — no runtime cost.

```c
offsetof(struct proc, run_list)  // → 0x18 (24 bytes, in our example)
```

### `container_of(ptr, type, member)`

Given a pointer to a member, recover the enclosing struct:

```c
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

Step by step:
1. Cast `ptr` to `char *` (so arithmetic is byte-granularity)
2. Subtract `offsetof(type, member)` — back up to the start of the struct
3. Cast result to `type *`

```c
struct list_head *node = ...; // points to some proc's run_list
struct proc *p = container_of(node, struct proc, run_list);
// Now p points to the enclosing proc.
```

This is pure pointer arithmetic — no runtime metadata, no virtual dispatch, no
type tags. It's also *completely type-unsafe* — if you pass the wrong member
name or wrong type, you get silent corruption. The compiler won't catch it. You
just get a garbage pointer and a debugging session. The tradeoff: zero overhead,
full trust in the programmer.

> **Linux note:** the kernel's `container_of` includes a `BUILD_BUG_ON` type
> check using `typeof` to verify that `ptr` is compatible with the member's type.
> We'll keep ours simple.

---

## Part 3: Doubly-Linked Circular List

### Structure

```
        ┌──────────────────────────────────────────┐
        │                                          │
        ▼                                          │
    ┌───────┐     ┌───────┐     ┌───────┐     ┌───────┐
    │ HEAD  │────>│ node1 │────>│ node2 │────>│ node3 │
    │(sent.)│<────│       │<────│       │<────│       │
    └───────┘     └───────┘     └───────┘     └───────┘
        |                                          ▲
        │                                          │
        └──────────────────────────────────────────┘
```

Key properties:
- **Circular** — the last node's `next` points back to `head`; head's `prev`
  points to the last node. There is no NULL anywhere.
- **Sentinel head** — the head is a `list_head` that isn't inside any container
  struct. It marks "beginning/end" of the list. An empty list is the head
  pointing to itself in both directions.
- **Uniform operations** — because it's circular with a sentinel, insertion and
  deletion don't need special-case code for "first element" or "last element."
  Every insertion is "between two existing adjacent nodes." Every deletion is
  "bypass one node by connecting its neighbors."

### Why circular + sentinel?

Compare with a NULL-terminated list:

```c
// NULL-terminated: every operation checks for edge cases
void delete(struct node **head, struct node *target) {
    if (*head == target) {        // special case: deleting the head
        *head = target->next;
        if (*head) (*head)->prev = NULL;
    } else {                      // general case
        target->prev->next = target->next;
        if (target->next) target->next->prev = target->prev;
    }
}
```

With circular + sentinel:

```c
// Circular: one case handles everything
void delete(struct list_head *entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
}
```

No NULL checks, no head-pointer update, no edge cases. The sentinel always
exists, so `prev` and `next` are never NULL on a properly-linked node.

> **What stops you from deleting the sentinel itself?** Nothing — there's no
> runtime guard. If you call `list_del(&head)`, you corrupt the list (lose
> your anchor). Linux takes the same approach: trust the programmer. The API
> makes accidental sentinel deletion unlikely — `list_for_each_entry` never
> yields the sentinel, so you'd have to go out of your way. The kernel
> philosophy is that illegal operations should crash loudly rather than be
> prevented by per-call runtime checks.

### Empty list invariant

```c
// Empty: head points to itself
head->next == head
head->prev == head
```

This falls out naturally from the circular design. Inserting the first element is
not a special case — it's "insert between head and head" (which are currently
each other's neighbors).

### Why the head is not inside a container struct

The head (sentinel) serves as the list's anchor point. It doesn't represent an
actual entry — it's the fixed point you start iteration from and stop at.

```c
LIST_HEAD(run_queue);  // declares a standalone list_head, not inside any struct

// Iteration starts at head->next (first real entry) and stops when we
// cycle back to &run_queue (the sentinel).
```

### The `__list_add` pattern

All insertions go through one internal helper that wires a new node between
two adjacent nodes:

```
Before:    prev ◀──────▶ next
After:     prev ◀──▶ new ◀──▶ next
```

`list_add(new, head)` inserts between `head` and `head->next` → front.
`list_add_tail(new, head)` inserts between `head->prev` and `head` → back.

Both are one-liners that call `__list_add` with different arguments. This is the
"uniform operations" payoff of the circular design.

### Iteration: `list_for_each_entry`

The most common pattern — walk a list and get enclosing structs:

```c
struct proc *p;
list_for_each_entry(p, &run_queue, run_list) {
    // p points to each proc in the run queue, in order
    kprintf("pid %d\n", p->pid);
}
```

Internally this is:
1. Start: `p = container_of(head->next, struct proc, run_list)`
2. Continue while: `&p->run_list != head` (haven't cycled back to sentinel)
3. Advance: `p = container_of(p->run_list.next, struct proc, run_list)`

### Safe iteration

If you might delete the current entry during iteration, you need
`list_for_each_entry_safe` — it saves the next pointer *before* the loop body
executes, so unlinking the current node doesn't lose your place.

---

## Part 4: Hash Table

### Why buckets need a list at all

A hash table is an array of buckets, indexed by `hash(key) % num_buckets`. If
every key mapped to a unique bucket, you'd just index the array and be done — no
list needed. But **collisions** happen: different keys hash to the same bucket
index. You need somewhere to put all entries that land in the same bucket.

Two strategies exist:
- **Separate chaining** (what we use) — each bucket holds a linked list. On
  collision, prepend to the bucket's chain. Deletion is O(1).
- **Open addressing** — on collision, probe other array slots. No lists, but
  deletion requires tombstones, and performance degrades as load factor rises.

We use separate chaining because intrusive `list_head` fits naturally, deletion
is trivial (already O(1) from our doubly-linked list), and load factor > 1.0 is
handled gracefully (chains just get longer). With a good hash and enough buckets,
average chain length = n/buckets, so lookup is O(1) amortized.

### Our approach: reuse `list_head` for bucket chains

Since we already have `list_head` with O(1) insertion and removal, we can reuse
it directly for hash bucket chains. Each bucket is simply a `list_head` sentinel,
and entries embed a `list_head` for hash membership — same primitive, no new
types needed.

```c
#define PID_HASH_BITS 6
struct list_head pid_table[1 << PID_HASH_BITS];   // 64 buckets, each a list_head

struct proc {
    int pid;
    struct list_head all_list;    // global process list
    struct list_head run_list;    // run queue
    struct list_head pid_link;    // hash table bucket chain
};
```

Insert:
```c
int bucket = hash_int(p->pid) & (HT_SIZE(PID_HASH_BITS) - 1);
list_add(&p->pid_link, &pid_table[bucket]);
```

Remove:
```c
list_del(&p->pid_link);   // O(1), no need to know which bucket
```

This is the approach FreeBSD and Windows take — standard doubly-linked lists for
everything, including hash bucket chains. Simple, uniform, and already proven.

### How Linux does it differently: `hlist`

> Linux introduces a separate type (`hlist_head` / `hlist_node`) to save memory
> on the bucket array. Each `hlist_head` is a single pointer (8 bytes) vs our
> `list_head` sentinel (16 bytes). With 64 buckets, that's 512 vs 1024 bytes —
> half the memory for the array.
>
> To get O(1) removal without a full `prev` pointer, `hlist_node` uses a
> **pprev trick**: `pprev` is a pointer to the *pointer* that references this
> node (either `&head->first` or `&prev_node->next`). Removal becomes
> `*(node->pprev) = node->next` — one indirection handles both "first in bucket"
> and "middle of bucket" uniformly. This is the same pointer-to-pointer pattern we 
> used in `slab_destroy` (Round 4-2), where `struct page **ps` walks the slab
> list to find the pointer that references the target slab, then unlinks via
> `*ps = slab->next_slab`. The difference: in our slab code, we *scan* to find
> that pointer (O(n)); in hlist, each node *stores* its own pprev at insert time,
> making removal O(1) without scanning.
>
> The tradeoff: more subtle pointer scheme, a second set of types/macros, but
> less memory when you have many hash tables with many buckets (Linux has dozens).
> At our scale (one PID table, <100 processes), the savings are negligible.
> We choose simplicity — one list primitive for everything.

### Hash function

We need to turn a PID (integer) into a bucket index. The standard approach is
**multiplicative hashing**:

**Step 1 — multiply** the key by a large odd constant:

```
product = key × 0x61C8864680B583EBull
```

This constant is `2^64 / φ` (golden ratio hash) — what Linux uses in
`include/linux/hash.h`. It's not cryptographic; it just ensures sequential keys
(PID 1, 2, 3, ...) spread across buckets rather than clustering. The constant is
large (fills most of 64 bits) and odd (guarantees the multiplication is
invertible mod 2^64, so no two inputs produce the same output).

The multiplication intentionally overflows — unsigned overflow in C is
well-defined (wraps mod 2^64), and the wrap-around is part of the mixing.

**Step 2 — extract a bucket index** from the 64-bit product. Two ways:

```
Shift variant:  bucket = product >> (64 - bits)       // extract high bits
Mask variant:   bucket = product & (num_buckets - 1)  // extract low bits
```

Same product, same constant — the only difference is which bits you keep. The
high bits have slightly better distribution because multiplication carries
propagate upward — the high bits are influenced by all bits of the key, while
the low bits are mostly influenced by the low bits of the key. In practice the
difference is small for our scale (sequential PIDs, tens of processes).

We use the mask variant — it's conventional (Linux's hashtable API does the same)
and `num_buckets - 1` is always available since our table sizes are powers of 2.

Why multiplicative hashing works well:
- Sequential inputs produce well-distributed outputs (unlike plain `key %
  num_buckets` which, with power-of-2 size, just uses the low bits of the key
  directly — no mixing at all)
- Single multiplication — fast on modern hardware
- No division, no branching

> **Why power-of-2 bucket counts, not primes?** With modulo hashing (`key % n`),
> a prime bucket count avoids clustering when keys share common factors with n.
> But multiplicative hashing gets its distribution from the multiply itself — the
> bucket count just slices the output. Power-of-2 lets us use `& (size - 1)`
> (single AND instruction) instead of expensive division. Java's HashMap made
> the same switch from prime to power-of-2 sizes once it adopted a better hash
> function.

### Lookup pattern

There's no built-in "find by key" function — the caller writes the lookup loop
because only the caller knows what the key is and how to compare:

```c
struct proc *find_proc(int pid) {
    int bucket = hash_int(pid) & (HT_SIZE(PID_HASH_BITS) - 1);
    struct proc *p;
    list_for_each_entry(p, &pid_table[bucket], pid_link) {
        if (p->pid == pid)
            return p;
    }
    return NULL;
}
```

This is different from Java's `map.get(key)` which internalizes comparison via
`equals()`. In C intrusive style, the hash table is *just* the bucket array +
link management. Key comparison is the caller's job. This is more code at the
call site, but it's completely flexible — you could hash on any field or
combination of fields.

---

## Part 5: Putting It All Together

Let's trace a concrete example showing how lists and hash table work together.
Four processes a, b, c, d — all exist, but only a and c are runnable:

```c
struct proc {
    int pid;
    struct list_head all_list;    // link for global list
    struct list_head run_list;    // link for run queue
    struct list_head pid_link;    // link for hash table bucket chain
};

LIST_HEAD(all_procs);                          // sentinel for global list
LIST_HEAD(run_queue);                          // sentinel for run queue
#define PID_HASH_BITS 2                        // 4 buckets (tiny, for illustration)
struct list_head pid_table[1 << PID_HASH_BITS]; // each bucket is a list_head sentinel

struct proc a = { .pid = 1 };   // runnable
struct proc b = { .pid = 2 };   // sleeping
struct proc c = { .pid = 3 };   // runnable
struct proc d = { .pid = 4 };   // sleeping
```

### The lists

```
all_procs list (all 4 procs, linked via .all_list):
    all_procs <-> a.all_list <-> b.all_list <-> c.all_list <-> d.all_list <-> (back to all_procs)

run_queue list (only runnable procs, linked via .run_list):
    run_queue <-> a.run_list <-> c.run_list <-> (back to run_queue)
```

b and d's `.run_list` fields are not in any list (NULL-poisoned or never added).

Key observations:
- Each `list_head` field connects to other `list_head` fields **of the same
  name** in other procs. `a.all_list.next` points to `&b.all_list`, NOT to `&b`
  and NOT to `&b.run_list`.
- The two `list_head` fields inside a single proc participate in **completely
  independent circular chains**. They share no pointers with each other.
- To recover `struct proc *` from a list node pointer, use `container_of` —
  e.g., `container_of(a.all_list.next, struct proc, all_list)` gives you `&b`.

### The hash table

All 4 procs are in the hash table regardless of state — it's purely for "given a
PID, find the proc." Suppose `hash_int` distributes them across 4 buckets like:

```
pid_table[0]:  (empty — sentinel points to itself)
pid_table[1]:  <-> d.pid_link <-> a.pid_link <-> (back to pid_table[1])   (pid 4 and 1 collide)
pid_table[2]:  <-> b.pid_link <-> (back to pid_table[2])                  (pid 2 alone)
pid_table[3]:  <-> c.pid_link <-> (back to pid_table[3])                  (pid 3 alone)
```

Key observations:
- **Same `list_head` everywhere** — each bucket is a circular list just like
  `all_procs` or `run_queue`. No new types. Same `list_add`, `list_del`,
  `list_for_each_entry` work here.
- **Collisions are handled by chaining.** pid 1 and pid 4 land in the same
  bucket — you walk the chain comparing keys until you find a match.
- **O(1) removal without knowing the bucket.** `list_del(&a.pid_link)` unlinks
  via prev/next — never needs to know which bucket `a` is in.
- **The hash table doesn't track process state.** All procs are in it. The lists
  track "all procs" and "runnable procs"; the hash table tracks identity (PID
  lookup).

### How the three structures cooperate

| Operation | Structure used | Complexity |
|-----------|---------------|------------|
| Find proc by PID (kill, waitpid) | `pid_table` hash | O(1) amortized |
| Pick next process to run | `run_queue` list (pop front) | O(1) |
| Enumerate all procs (ps) | `all_procs` list | O(n) |
| Process yields/sleeps | Remove from `run_queue` | O(1) |
| Process wakes up | Add to `run_queue` tail | O(1) |
| Process exits | Remove from all three | O(1) each |

One proc, three embedded link fields, three independent data structures, zero
external allocation.

---

## Part 6: bobchouOS vs Linux

| Aspect | Linux | bobchouOS |
|--------|-------|-----------|
| Doubly-linked list | `<linux/list.h>` | `list.h` (same API) |
| Hash bucket chains | `hlist_head`/`hlist_node` (pprev trick) | `list_head` (same type as all other lists) |
| Bucket head memory | 8 bytes per bucket | 16 bytes per bucket |
| `container_of` type check | `BUILD_BUG_ON` + `typeof` validation | Simple subtraction only |
| `list_del` poison | `LIST_POISON1/2` (0xdead addresses) | NULL (simpler, still detectable) |
| RCU-safe variants | `list_for_each_entry_rcu`, `hlist_for_each_entry_rcu` | Not needed (no SMP, no RCU) |
| Hash function | `hash_long()` in `<linux/hash.h>` | `hash_int()` (same algorithm) |
| Lock-free reads | RCU + `rcu_dereference` | Single-hart, no concurrency concern |

We use `list_head` for everything — including hash bucket chains — matching the
FreeBSD/Windows approach. One list primitive, no extra types. Linux's hlist
optimization saves memory on bucket arrays at the cost of a second set of
types/macros; we skip that complexity at our scale.

---

## Quick Reference

### list.h — Doubly-linked circular list

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| `INIT_LIST_HEAD(head)` | O(1) | Head → self (empty) |
| `list_add(new, head)` | O(1) | Insert at front |
| `list_add_tail(new, head)` | O(1) | Insert at back |
| `list_del(entry)` | O(1) | Unlink, poison prev/next |
| `list_empty(head)` | O(1) | head->next == head? |
| `list_for_each_entry(pos, head, member)` | O(n) | Typed iteration |
| `container_of(ptr, type, member)` | O(1) | Node → enclosing struct |

### hashtable.h — Hash table (list_head buckets)

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| `hash_init(ht, bits)` | O(2^bits) | INIT_LIST_HEAD each bucket |
| `hash_add(ht, node, bits, hash)` | O(1) | list_add into bucket |
| `hash_del(node)` | O(1) | list_del (same as any list removal) |
| Lookup by key | O(chain length) | list_for_each_entry + compare |
| `hash_int(val)` | O(1) | Multiplicative hash |
