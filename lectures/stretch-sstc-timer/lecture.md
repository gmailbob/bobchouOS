# Stretch Lecture: The SSTC Timer — Retiring the Mini-SBI Middleman

> **Where we are**
>
> Back in Lecture 2-3 we built the timer heartbeat, and we faced a fork in
> the road. RISC-V machine timer interrupts (MTI) cannot be delegated to
> S-mode, so *something* in M-mode has to catch each tick and hand it down
> to the kernel. We laid out **three approaches** for solving this and
> promised to come back for the third one. This is that return trip.
>
> **Approach 1 — what we actually built** — the "direct CLINT + SSIP
> forwarding" trick: our own tiny M-mode handler (`m_vec.S`) catches the
> machine timer interrupt, raises a *supervisor software interrupt* (SSIP)
> to poke S-mode, and disarms the hardware timer. S-mode wakes up, sees
> `scause = 1` (a software interrupt — a small lie about what really
> happened), ticks, and re-arms the timer by making an `ecall` back into
> M-mode. Every single tick bounces through M-mode **twice**: once for the
> interrupt, once for the re-arm `ecall`.
>
> **Approach 2 — the SBI-firmware path** (we did *not* build this, but it
> is the bridge to approach 3). Real systems run firmware like OpenSBI in
> M-mode. The OS calls `sbi_set_timer(deadline)` via `ecall`; firmware
> programs the timer and, crucially, delivers the tick as a *real*
> supervisor timer interrupt — `scause = 5` (`IRQ_S_TIMER`), with the OS
> enabling `sie.STIE` and handling `IRQ_S_TIMER`. So approach 2 already
> uses the **honest cause bit** that approach 1 faked. What it still has
> is the M-mode middleman: every tick and every re-arm bounces through
> firmware. **Take note of this** — it means the `scause 1 → 5` change is
> *not* what SSTC uniquely gives us; approach 2 had it too. SSTC's one
> novel contribution is deleting the middleman entirely.
>
> **Approach 3 — the modern path** — uses the **SSTC extension**
> (Supervisor Timer Compare). It gives S-mode its own timer-compare CSR,
> `stimecmp`, that S-mode writes *directly*. Hardware compares
> `time >= stimecmp` and raises a real *supervisor timer interrupt* (STIP)
> with no M-mode involvement at all. Like approach 2 the kernel sees
> `scause = 5` and enables `sie.STIE` — but unlike approach 2, it re-arms
> by writing `stimecmp` itself. **Zero privilege crossings** on the timer
> hot path. That — not the cause bit — is the win.
>
> This lecture migrates bobchouOS from approach 1 to approach 3. By the
> end you will understand:
>
> - Why S-mode needs two M-mode *permission bits* (`menvcfg.STCE` to
>   write `stimecmp`, `mcounteren.TM` to read `time`) — the one piece of
>   M-mode that survives
> - The difference between a **latched** pending bit (SSIP, which you
>   clear) and a **level** pending bit (STIP, which you *outrun*)
> - Why the timer's `scause` flips from 1 (software) to 5 (timer) — and
>   why that flip is really us catching up to approach 2, *not* the thing
>   SSTC uniquely buys us
> - What gets *deleted*: the `m_vec.S` timer logic, `sbi_set_timer()`,
>   the `mie.MTIE` enable, the boot-time timer disarm
> - Why shutdown *stays* in M-mode even though the timer leaves — the
>   "delete a workaround, respect a boundary" principle
> - Why doing this **before** the multi-hart scheduler (Phase 9) keeps
>   that code clean
>
> **Prerequisite reading:** Lecture 2-3 (the three approaches, the CLINT,
> `mtime`/`mtimecmp`, the SSIP forwarding trick). This lecture assumes you
> remember the approach-1 machinery we are now tearing out.
>
> **xv6 book coverage:** None directly — xv6 uses approach 1 (its
> `timervec` is the model our `m_vec.S` timer path was based on). SSTC
> postdates the xv6 book. This is bobchouOS going beyond xv6 toward what
> modern Linux on RISC-V actually does.

---

## Part 1: Why Bother? The Cost of the Middleman

Our approach-1 timer works. It has worked since Lecture 2-3, through the
whole scheduler, sleep/wakeup, and process lifecycle. So why change it?

Three reasons, in increasing order of importance.

### Reason 1: Performance — two M-mode bounces per tick

Trace one timer tick under approach 1:

```
  hardware: mtime >= mtimecmp
       │
       ▼  (1) MACHINE timer interrupt — trap to M-mode
  m_vec.S: raise SSIP, set mtimecmp = -1 (disarm)
       │
       ▼  (2) mret back to S-mode; SSIP now pending
  trap.c: scause=1, clear SSIP, tick, set need_resched
       │
       ▼  (3) scheduler picks next proc, computes deadline
  proc.c: sbi_set_timer(deadline)  ──ecall──▶  M-mode
       │                                         │
       │                                  m_vec.S: write mtimecmp
       ▼  (4) mret back to S-mode, run the process
```

Two full privilege round-trips per tick (steps 1–2 and steps 3–4). At
100 ticks/second that is 200 mode transitions per second per hart, every
one of them pure overhead — saving and restoring M-mode scratch
registers to do work S-mode could have done itself.

Under SSTC the same tick is:

```
  hardware: time >= stimecmp
       │
       ▼  (1) SUPERVISOR timer interrupt — trap straight to S-mode
  trap.c: scause=5, disarm (stimecmp = -1), tick, set need_resched
       │
       │  (2) scheduler computes deadline, csrw stimecmp
       ▼      run the process — no trap, no ecall
```

Zero M-mode transitions. The CLINT MMIO write and the `ecall` both
vanish.

> On QEMU the saved cycles are invisible — emulation is not where you
> measure interrupt latency. The point is *architectural*, not
> benchmark-driven: on real silicon, a timer tick that never leaves
> S-mode is meaningfully cheaper, and at high tick rates (1000 Hz
> kernels, or tickless kernels reprogramming the timer constantly) it
> adds up. We make the change because it is *correct and modern*, and
> because of reasons 2 and 3.

### Reason 2: Honesty — `scause = 5` means what it says

Under approach 1, a timer tick arrives as `scause = 1`, a *software*
interrupt. That is a deliberate fiction: there is no software interrupt:
M-mode raised SSIP purely as a signalling trick because it had no other
way to interrupt S-mode. Anyone reading the trap handler has to know the
backstory to understand why the timer lives under the `IRQ_S_SOFT` case.

Under SSTC, a timer tick arrives as `scause = 5` (`IRQ_S_TIMER`) — a
*supervisor timer interrupt*, which is exactly what it is. The code
stops lying. The `IRQ_S_TIMER` constant has been sitting in `riscv.h`
since Lecture 2-3, defined but unused, waiting for this day.

### Reason 3: Per-hart cleanliness — this is why we do it *before* Phase 9

This is the real driver, and it is about *sequencing*.

`stimecmp` is a CSR. CSRs are inherently **per-hart** — each hart has its
own, addressed by simply executing `csrw stimecmp` on that hart. There is
no array, no index, no shared address.

Contrast the approach-1 machinery. The CLINT's `mtimecmp` is **MMIO**,
and it is one big array indexed by hart ID:

```
  CLINT_MTIMECMP(hart) = CLINT_BASE + 0x4000 + 8 * hart
```

Today, with one hart, we hardcode `CLINT_MTIMECMP(0)` everywhere. The
moment Phase 9 boots multiple harts, every one of those sites would have
to become `CLINT_MTIMECMP(this_hart)` — and the SSIP forwarding would
need per-hart routing too (which hart's `m_vec` raises which hart's
SSIP). We would build all that per-hart MMIO plumbing in Phase 9... only
to throw it away if we ever adopted SSTC.

By switching to SSTC **now**, the multi-hart scheduler in Phase 9 just
writes `stimecmp` on whatever hart it is running on. No indexing, no
routing, no shared CLINT state. The per-hart-ness is free, baked into
the CSR. We pay the migration cost once, on a single-hart kernel where
it is easy to reason about, instead of tangled up with multi-core bring-up.

> **Decision recorded:** do SSTC *before* Phase 9, not after. The
> alternative — carry approach 1 into multi-hart, then refactor — means
> writing per-hart `mtimecmp[hart]` indexing and SSIP routing that the
> SSTC migration would immediately delete. Cheaper to migrate first.

---

## Part 2: The One Thing M-mode Still Must Do — `menvcfg.STCE`

Here is the gotcha that surprises everyone the first time. The whole
*point* of SSTC is that S-mode talks to the timer directly. So you would
expect M-mode to be entirely out of the picture. It is not — not quite.

S-mode is **forbidden** from accessing `stimecmp` unless M-mode has first
flipped a permission bit: **`menvcfg.STCE`** (detailed below). If
`STCE = 0` and S-mode executes `csrw stimecmp, t0`, the hart does *not*
set a timer — it takes an **illegal-instruction exception** (`scause = 2`).

So SSTC does not remove M-mode from the timer; it reduces M-mode's role
to **two permission bits, set once, at boot.** After those flips, M-mode
never touches the timer again. The two gates are independent and easy to
conflate:

- **`menvcfg.STCE`** (Machine Environment Configuration, the STCE
  "Supervisor Timer Counter Enable" bit, bit 63) — lets S-mode **write
  `stimecmp`** (arm the timer). The focus of this part.
- **`mcounteren.TM`** (Machine Counter-Enable, the TM "time" bit, bit 1)
  — lets S-mode **read the `time` CSR** (`rdtime`), which is how we now
  read the clock. Separate permission, separate bit. We never needed it
  before because the old `read_mtime()` read the CLINT over MMIO,
  sidestepping the gate.

Miss either and the corresponding instruction traps as illegal: `STCE`
off → `csrw stimecmp` faults; `TM` off → `rdtime` faults.

```
  ┌─────────────── M-mode (boot, once) ────────────────┐
  │  menvcfg.STCE = 1    "S-mode may WRITE stimecmp"   │
  │  mcounteren.TM = 1   "S-mode may READ time"        │
  └────────────────────────┬───────────────────────────┘
                           │ mret
                           ▼
  ┌─────────────── S-mode (forever after) ─────────────┐
  │  csrw stimecmp, deadline   ← arm (needs STCE)      │
  │  rdtime  → read_time()     ← clock (needs TM)      │
  │  (time >= stimecmp) → STIP → trap, tick            │
  └────────────────────────────────────────────────────┘
```

### Why does the hardware require M-mode opt-in at all?

It seems redundant — why not just always let S-mode use its own timer?
Two reasons, both about M-mode keeping ultimate authority:

1. **Portability / feature negotiation.** Not every chip implements
   SSTC. `menvcfg` is the standard place where M-mode firmware advertises
   "this feature is available and turned on." A portable kernel can
   probe it. (We skip probing — we *know* QEMU has it — but the bit is
   the mechanism a real kernel would check.)
2. **Virtualization layering.** There is a parallel `henvcfg.STCE` for
   the hypervisor extension: a hypervisor gates whether a *guest* S-mode
   (VS-mode) gets a direct timer. The `*envcfg` family exists precisely
   so each privilege level can gate the feature for the level below it.
   We have no hypervisor, but the bit lives in `menvcfg` for this reason.

> Both gates live in M-mode CSRs that are registers full of such bits:
>
> - **`menvcfg`** (Machine Environment Configuration) gates *features*:
>   `STCE` for the SSTC timer, `CBZE`/`CBCFE`/`CBIE` for cache-block
>   instructions, `FIOM` for fence-of-I/O ordering.
> - **`mcounteren`** (Machine Counter-Enable) gates *counter reads*: `TM`
>   for the `time` CSR, `CY` for `cycle`, `IR` for `instret`, plus the
>   `hpmcounter` performance counters.
>
> Both follow the general RISC-V pattern: a more-privileged mode holds
> *enable* bits that unlock capabilities for the mode below. The privilege
> drop SSTC gives us is real, but it is *delegated*, not *unguarded* —
> M-mode says "you may," then steps back.

### The bit-63 trap (literal trap, a coding one)

`MENVCFG_STCE` is `1 << 63`. You cannot load that with a single `li`
into a register from a 12-bit immediate, and you cannot `csrs` it from an
immediate either. The boot code must build it in a register:

```asm
    li    t0, 1
    slli  t0, t0, 63       # t0 = 1 << 63 = MENVCFG_STCE
    csrs  menvcfg, t0      # set bit 63, leave the rest alone
```

Using `csrs` (set bits) rather than `csrw` (write whole register) means
we touch only bit 63 and leave any other `menvcfg` bits untouched.

---

## Part 3: Latched vs. Level — Why You Don't *Clear* STIP

This is the conceptual heart of the migration, and the source of the
single most common SSTC bug. The pending bit changes character.

### SSIP is latched — you clear it

Under approach 1, M-mode *raises* SSIP by writing it. It is a software
bit: somebody set it, somebody must clear it. Our handler does exactly
that ([trap.c], the `IRQ_S_SOFT` case):

```c
csrw(sip, csrr(sip) & ~SIP_SSIP);   /* "handled — stop pinging me" */
```

If we forgot this line, `sret` would return to the interrupted code with
SSIP still pending, and we would trap right back in — an infinite loop.
The clear is mandatory and explicit.

### STIP is a level signal — you can't clear it, you outrun it

Under SSTC, STIP is **not** a latch you own. It is the live output of a
hardware comparator:

```
  STIP = (time >= stimecmp)     ← recomputed every cycle
```

As long as that comparison is true, STIP is high. Writing `csrc sip,
STIP` does nothing useful — the hardware recomputes STIP on the very next
cycle and sets it high again, because `time` is still `>= stimecmp`.

The only way to lower STIP is to **make the comparison false** — set
`stimecmp` to some future value that `time` has not yet reached. So in
the timer handler, *the act of disarming or re-arming the timer is what
dismisses the interrupt.* There is no separate "clear pending" step.

```
   handler entry:  time >= stimecmp  →  STIP high (that's why we're here)
        │
        │  csrw stimecmp, <future value>
        ▼
   now:  time <  stimecmp  →  STIP low.  Dismissed.
```

★ The bug this invites: if your handler returns **without** moving
`stimecmp` past `time`, STIP is still high, and `sret` drops you
straight back into the same timer trap. An infinite interrupt storm. The
approach-1 code was immune (it cleared SSIP); the SSTC code must take
positive action to lower STIP. This is genuinely a *new* failure mode.

> This is the textbook **edge-triggered vs. level-triggered**
> distinction, made concrete in two CSRs. SSIP is edge-ish — a latch you
> toggle. STIP is level — a comparator output you can only influence by
> moving its input. It is the same model as the PLIC external interrupts
> you met in Round 7-1: you don't "clear" a level interrupt, you remove
> its *cause* (there, by servicing the device; here, by pushing the
> deadline forward).

### Our disarm policy: write `stimecmp = -1`

We deliberately mirror the **disarm-then-reschedule** shape our kernel
already uses. That shape is not original to Lecture 2-3 — back then the
M-mode handler re-armed *itself* periodically (`mtimecmp += interval`), a
free-running tick with no scheduler involved. The `mtimecmp = -1` disarm
arrived later, with the Phase 5 scheduler: once S-mode needed to *choose*
each deadline (quantum expiry vs. the earliest sleeper), the handler
stopped re-arming and instead disarmed by writing `mtimecmp = -1` (max
`uint64`), leaving the scheduler to set the real deadline. That is the
shape we carry over here — we do the identical thing one privilege level
down:

```c
case IRQ_S_TIMER:
    csrw(stimecmp, (uint64)-1);   /* disarm: time can never reach 2^64-1,
                                   * so STIP drops and stays down */
    wake_expired_sleepers();
    if (this_cpu()->proc)
        this_cpu()->need_resched = 1;
    break;
```

Then the scheduler re-arms with the real deadline, exactly as it does
today. `-1` is self-documenting: *"disarmed — threshold so high it never
trips."* It is the same idiom as the `mtimecmp = -1` the scheduler-era
code already uses, just on `stimecmp`.

| | Approach 1 today (mini-SBI + scheduler) | SSTC |
|---|---|---|
| Disarm (in handler) | M-mode writes `mtimecmp = -1` | S-mode writes `stimecmp = -1` |
| Re-arm (in scheduler) | `sbi_set_timer(deadline)` ecall | `csrw(stimecmp, deadline)` |
| Dismiss the interrupt | clear SSIP explicitly | (implied by the disarm — outrun it) |

> **Why disarm in the handler at all, instead of just re-arming once in
> the scheduler?** Because the disarm happens with interrupts *off* (we
> are mid-trap), there is never a window where STIP is high while
> interrupts are enabled. If we instead left STIP high and relied on the
> scheduler to re-arm "soon," any interrupt-enable in between would storm.
> Disarming in the handler closes that hazard structurally. The cost is
> one extra CSR write per tick (disarm, then re-arm) — cheap insurance,
> and a deliberate choice, not an oversight.

---

## Part 4: What Changes — and Two Things the Diff Won't Tell You

The mechanical edits — delete the `m_vec.S` timer paths and
`sbi_set_timer`, swap `csrw stimecmp` for the ecall in `proc.c`, move the
`trap.c` case from `IRQ_S_SOFT` to `IRQ_S_TIMER`, flip `SSIE`→`STIE` in
`main.c`, set the two boot gates in `entry.S` — are all in the skeleton
commit, and the diff reads cleanly on its own. The Quick Reference at the
end lists every file touched. Two points, though, are *reasoning* the
diff cannot show you.

### Why there is no boot-time `stimecmp` disarm

Approach 1's boot code wrote `mtimecmp = -1` to keep the timer quiet
until armed. The SSTC port has *no* equivalent line — and that is correct,
not an omission:

```
  stimecmp powers up at 0, so STIP is high from reset.
  BUT STIP can only DELIVER an interrupt when two S-mode gates open:
      sie.STIE   (set late in kmain)
   && sstatus.SIE (set per-thread via intr_on())
  Both are off through all of boot. So STIP sits harmlessly high until
  the scheduler writes a real stimecmp — which happens BEFORE any thread
  runs intr_on(). The power-on STIP is overwritten before it can fire.
```

So the old disarm has no successor. Pure deletion.

### Which retired-looking definitions are *not* actually dead

After the migration several `riscv.h` constants stop being referenced. It
is tempting to delete them all. Don't — they fall into two groups:

- **Genuinely retired:** `CLINT_MTIMECMP`, `CLINT_MTIME` (replaced by
  `stimecmp` and the `time` CSR), plus `MIE_MTIE` / `IRQ_M_TIMER`
  (M-mode fields no timer interrupts). We drop the two address macros and
  keep the M-mode bit defs as hardware reference.
- **Only *resting*:** `SIP_SSIP`, `SIE_SSIE`, `IRQ_S_SOFT`. The
  supervisor *software* interrupt is unused now, but Phase 9 revives it
  as the **inter-processor interrupt (IPI)** mechanism — one hart poking
  another. The software interrupt was always *meant* for IPIs; approach 1
  borrowed it as a fake timer signal, and Phase 9 gives it back to its
  real job. Deleting these now just means re-adding them in two phases.

`CLINT_BASE` stays for the same reason — Phase 9 IPIs are sent by writing
the CLINT's `msip[hart]` registers.

---

## Part 5: Why Shutdown Stays in M-mode

After the timer leaves, `m_vec.S` handles only one thing: the
`SBI_SHUTDOWN` ecall, which writes the QEMU test-finisher device to halt
the machine. We *could* delete `m_vec` entirely and have S-mode write
that MMIO device directly — our PMP grants S-mode full physical access,
so it would work.

We keep it in M-mode anyway. The reason is a clean principle:

> **Delete a workaround; respect a boundary.**

The timer was in M-mode only as a **workaround** for missing hardware:
pre-SSTC, S-mode literally could not reach a timer-compare register, so
M-mode forwarded on its behalf. SSTC fixes the hardware gap, so the
workaround is *deleted*.

Shutdown is in M-mode by **design**. On real systems, power control —
shutdown, reboot, suspend — is genuinely a firmware (M-mode) job. The
real SBI specification has an entire **SRST** (System Reset) extension
for exactly this, precisely because the OS is *not* supposed to poke
platform-specific power MMIO directly. Linux shuts down by calling
`sbi_system_reset()`, not by writing a hardcoded device address.

So keeping `SBI_SHUTDOWN` as an ecall is the *architecturally honest*
choice. It mirrors how real kernels work, and it makes a nice teaching
point: after the timer migration, M-mode's only remaining job is the
thing that is *genuinely* privileged. Look how small the M-mode surface
gets when the workaround is gone.

```
  M-mode responsibilities
  ───────────────────────
  before SSTC:  boot setup
                timer forwarding (MTI → SSIP)   ← workaround, DELETED
                timer re-arm (SBI_SET_TIMER)    ← workaround, DELETED
                shutdown (SBI_SHUTDOWN)         ← design boundary, KEPT
  after  SSTC:  boot setup (+ menvcfg.STCE)
                shutdown (SBI_SHUTDOWN)         ← all that's left
```

---

## Part 6: How Real Systems Got Here

The path bobchouOS is walking is the same one the RISC-V ecosystem
walked, in order:

| Era | Timer mechanism | S-mode sees |
|---|---|---|
| Early / no firmware | Direct CLINT + SSIP forwarding | scause=1 (software) — *our approach 1* |
| Mainstream | SBI firmware (OpenSBI) `sbi_set_timer` + STIP | scause=5 (timer) — *approach 2* |
| Modern | SSTC `stimecmp`, no firmware bounce | scause=5 (timer) — *approach 3, this lecture* |

The SBI legacy `sbi_set_timer` call was eventually folded into the
standard TIME extension, and then SSTC made even *that* unnecessary on
capable hardware — S-mode programs its own timer. At the same time, the
ecosystem *added* SRST for system reset. The industry pulled the timer
*down* into S-mode while keeping reset *up* in firmware: the exact
"delete a workaround, respect a boundary" split we apply in Part 5.

---

## Quick Reference

### CSRs and bits

| Name | Address / bit | Mode | Role |
|---|---|---|---|
| `stimecmp` | `0x14D` | S | Timer compare; S writes deadline. STIP set when `time >= stimecmp` |
| `time` | `0xC01` | S (read) | Counter, read via `rdtime` — what `read_time()` now uses |
| `menvcfg.STCE` | bit 63 | M | Gate: S-mode may **write** `stimecmp`. Off → illegal-inst trap |
| `mcounteren.TM` | bit 1 | M | Gate: S-mode may **read** `time` (`rdtime`). Off → illegal-inst trap |
| `sie.STIE` | bit 5 | S | Enable supervisor timer interrupts |
| `sip.STIP` | bit 5 | S | Timer pending (level; read-only under SSTC) |
| `scause` | `=5` | S | `IRQ_S_TIMER` — the cause the timer now arrives as |

### The two ideas to remember

1. **M-mode's only remaining timer duty is two boot gates** —
   `menvcfg.STCE` (S-mode may *write* `stimecmp`) and `mcounteren.TM`
   (S-mode may *read* `time`). Set once, then M-mode is out. Miss either
   and the corresponding instruction traps as illegal. Don't conflate
   them: STCE for arming, TM for reading the clock.
2. **You outrun STIP, you don't clear it.** STIP = `(time >= stimecmp)`,
   recomputed every cycle. Disarm/re-arm by pushing `stimecmp` past
   `time` (`stimecmp = -1` to disarm). Forget to, and you storm.
