/*
 * proc.c — Process management for bobchouOS.
 *
 * Implements process creation, scheduling, yield, sleep/wakeup,
 * proc_exit, proc_wait, proc_kill, and per-CPU state.
 *
 * See Lectures 5-1 and 5-2.
 */

#include "proc.h"
#include "hashtable.h"
#include "kalloc.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "mem_layout.h"
#include "riscv.h"
#include "sbi.h"
#include "string.h"
#include "trapframe.h"
#include "vm.h"

/* --- Global state --- */

static LIST_HEAD(all_procs);
static LIST_HEAD(run_queue);
static DEFINE_HASHTABLE(pid_table, PID_HASH_BITS);

static struct cpu cpus[1];
static int next_pid = 0;

struct spinlock wait_lock;
struct spinlock run_queue_lock;
struct spinlock pid_lock;

/* "tsleep" = timed sleep (BSD term): a process blocked in sys_sleep(n)
 * until a wall-clock deadline. Distinct from event sleep (a proc waiting
 * on a wait_queue, e.g. child_wq, woken by another process). tsleep_lock
 * guards tsleep_list, the deadline-sorted queue of timer-sleepers. */
struct spinlock tsleep_lock;
struct proc *init_proc;

LIST_HEAD(tsleep_list); /* sorted by wake_time, earliest first */

/* --- PID allocation (self-contained locking) --- */

static int
alloc_pid(void) {
    spin_lock(&pid_lock);
    int pid = next_pid++;
    spin_unlock(&pid_lock);
    return pid;
}

/* --- Per-CPU accessors --- */

struct cpu *
this_cpu(void) {
    return &cpus[0];
}

struct proc *
this_proc(void) {
    struct proc *p = this_cpu()->proc;
    if (p)
        return p;
    panic("this_proc: no current process");
}

/* --- Run queue (self-contained locking) --- */

void
run_queue_add(struct proc *p) {
    spin_lock(&run_queue_lock);
    list_add_tail(&p->run_list, &run_queue);
    spin_unlock(&run_queue_lock);
}

/* --- Switch to scheduler --- */

/*
 * sched — switch from current process to scheduler.
 *
 * Two lock transitions happen across sched():
 *   Outbound: caller's p->lock crosses into the scheduler, which releases
 *             it just after its swtch-to-us returns (see scheduler()).
 *   Inbound:  before the scheduler swtch-es back to us, it re-acquires
 *             p->lock. So when sched() RETURNS, p->lock is HELD — the
 *             caller must release it (yield/wq_sleep do; proc_exit never
 *             returns so it doesn't).
 *
 * Precondition: caller holds p->lock (the golden rule).
 */
void
sched(void) {
    swtch(&this_proc()->context, &this_cpu()->scheduler);
}

/* --- Scheduler --- */

static struct proc *
pick_next(void) {
    if (list_empty(&run_queue))
        panic("pick_next: run queue empty");
    struct proc *p = list_first_entry(&run_queue, struct proc, run_list);
    list_del(&p->run_list);
    return p;
}

/*
 * scheduler — main scheduling loop (never returns).
 *
 * Precondition: interrupts are off (entered via swtch from a process
 * that held a lock with irqsave, or from kmain at boot before any
 * intr_on). Uses plain spin_lock throughout — safe because SIE stays
 * off for the entire scheduler loop (we never call intr_on).
 *
 * Each iteration:
 * 1. Release prev process's lock (it yielded holding p->lock)
 * 2. Pick next RUNNABLE process from run queue (under run_queue_lock)
 * 3. Acquire p->lock, set RUNNING, arm timer, swtch to it
 */
void
scheduler(void) {
    struct cpu *c = this_cpu();

    for (;;) {
        spin_lock(&run_queue_lock);
        struct proc *p = pick_next();
        spin_unlock(&run_queue_lock);

        spin_lock(&p->lock);
        p->state = PROC_RUNNING;
        c->proc = p;

        /* Arm timer for whichever comes first: quantum expiry or the earliest
         * sleeper's deadline. This gives precise sleep wakeup instead of
         * up-to-one-tick latency.
         *
         * LOCK ORDER NOTE: here we take tsleep_lock while holding p->lock
         * (p->lock -> tsleep_lock). Everywhere else the order is the reverse
         * (tsleep_lock -> p->lock: see sys_sleep, wake_expired_sleepers,
         * proc_kill). That inversion is an ABBA hazard. It is safe ONLY on a
         * single hart, because the scheduler runs with interrupts off and is
         * the sole holder of p->lock in this window, so no other context can
         * be mid-(tsleep_lock,p->lock) concurrently. Phase 9 (multi-core) must
         * fix this — e.g. snapshot the earliest deadline under tsleep_lock
         * before acquiring p->lock. */
        uint64 deadline = read_time() + TIMER_INTERVAL;
        spin_lock(&tsleep_lock);
        if (!list_empty(&tsleep_list)) {
            struct proc *sleeper = list_first_entry(&tsleep_list, struct proc, tsleep_link);
            if (sleeper->wake_time < deadline)
                deadline = sleeper->wake_time;
        }
        spin_unlock(&tsleep_lock);

        set_timer(deadline);

        /* swtch to p. p resumes wherever it last left off:
         *   - brand-new proc: at kthread_start / user_proc_start, which
         *     releases the p->lock we just acquired, then runs.
         *   - a proc that previously yielded/slept: inside its sched(),
         *     which returns into yield/wq_sleep where p releases p->lock.
         * Either way, p releases the lock we acquired above (the inbound
         * release). p runs freely, then re-acquires p->lock before its
         * next sched() back to us. */
        swtch(&c->scheduler, &p->context);

        /* p called sched() holding p->lock (acquired in yield/wq_sleep/
         * proc_exit). That is the outbound release — we do it here. */
        c->proc = NULL;
        spin_unlock(&p->lock);
    }
}

/*
 * yield — give up the CPU.
 * Called voluntarily by a process (SIE may be on), or by kernel_trap_ret
 * on timer preemption (SIE off). Uses irqsave to handle both safely.
 *
 * Postcondition: returns with interrupt state restored.
 */
void
yield(void) {
    struct proc *p = this_proc();
    unsigned long irq;
    spin_lock_irqsave(&p->lock, &irq);
    p->state = PROC_RUNNABLE;
    run_queue_add(p);
    sched();                               /* outbound: p->lock crosses to scheduler */
    spin_unlock_irqrestore(&p->lock, irq); /* inbound: p->lock held (scheduler re-acquired
                                              it before dispatching us); release and
                                              restore the saved interrupt state */
}

/*
 * proc_exit — terminate the current process.
 *
 * Reparents children to init, wakes parent, marks ZOMBIE, yields forever.
 * Never returns.
 */
void
proc_exit(int status) {
    struct proc *p = this_proc();

    if (p->pid <= 1)
        panic("idle or init exiting");

    unsigned long irq; /* dead — exit never returns, so irq is never restored */
    spin_lock_irqsave(&wait_lock, &irq);

    /* Reparent children to init */
    while (!list_empty(&p->children)) {
        struct proc *child = list_first_entry(&p->children, struct proc, sibling);
        child->parent = init_proc;
        list_del(&child->sibling);
        list_add_tail(&child->sibling, &init_proc->children);
    }

    /* Wake parent — it might be sleeping in wait() */
    wq_wake_one(&p->parent->child_wq);

    spin_lock(&p->lock);
    p->exit_status = status;
    p->state = PROC_ZOMBIE;
    spin_unlock(&wait_lock); /* plain unlock — still hold p->lock, can't restore irq */

    sched(); /* never returns */
    panic("zombie exit");
    (void)irq;
}

/*
 * free_proc — unlink a zombie from all containers and release its memory.
 *
 * Precondition: caller holds wait_lock (reaping is serialized with exit).
 *
 * User resources are freed only for user processes (kernel threads have no
 * pagetable/trapframe). Order matters: vma_free_all must run BEFORE
 * proc_free_pagetable, because it walks the page table to find and page_put
 * the leaf user pages; proc_free_pagetable then frees the table's own
 * (intermediate) pages. The trapframe is a separate kalloc'd page.
 */
static void
free_proc(struct proc *p) {
    list_del(&p->sibling);  /* off parent's children list */
    list_del(&p->all_list); /* off the global all_procs list */
    list_del(&p->pid_link); /* out of the pid hash table */

    if (p->pagetable) {                    /* user process? */
        vma_free_all(p);                   /* free user pages (leaf), clear PTEs */
        proc_free_pagetable(p->pagetable); /* free page-table pages themselves */
        kfree(p->trapframe);               /* free the trapframe page */
    }
    kfree((void *)p->kstack); /* every proc has a kernel stack */
    kmfree(p);                /* finally the proc struct */
}

/*
 * proc_wait — wait for a child to exit, reap it.
 *
 * Returns the child's PID (or -1 if no children exist).
 * Writes child's exit status to *status if non-NULL.
 *
 * Sleeps until a child exits if children exist but none are zombies yet.
 */
int
proc_wait(int *status) {
    struct proc *p = this_proc();
    unsigned long irq;

    spin_lock_irqsave(&wait_lock, &irq);

    for (;;) {
        if (list_empty(&p->children)) {
            spin_unlock_irqrestore(&wait_lock, irq);
            return -1;
        }

        struct proc *child;
        list_for_each_entry(child, &p->children, sibling) {
            spin_lock(&child->lock);
            if (child->state == PROC_ZOMBIE) {
                int pid = child->pid;
                if (status)
                    *status = child->exit_status;
                spin_unlock(&child->lock);
                free_proc(child);
                spin_unlock_irqrestore(&wait_lock, irq);
                return pid;
            }
            spin_unlock(&child->lock);
        }

        /* Check killed before sleeping — if we were woken by kill(),
         * proc_exit instead of sleeping again. */
        if (p->killed) {
            spin_unlock_irqrestore(&wait_lock, irq);
            proc_exit(-1);
        }

        /* Children exist but no zombie — sleep until one exits */
        wq_sleep(&p->child_wq, &wait_lock);
        /* wq_sleep releases wait_lock, sleeps, re-acquires it */
    }
}

/*
 * proc_kill — mark a process for termination.
 *
 * Sets p->killed = 1. If SLEEPING, wakes it so it can notice the flag.
 * Returns 0 on success, -1 if PID not found.
 *
 * Lock ordering: tsleep_lock → p->lock (same as sys_sleep and
 * wake_expired_sleepers). We acquire tsleep_lock first when the target
 * might be on the tsleep_list.
 */
int
proc_kill(int pid) {
    int bucket = hash_int(pid) & (HT_SIZE(PID_HASH_BITS) - 1);
    struct proc *p;
    int found = 0;

    list_for_each_entry(p, &pid_table[bucket], pid_link) {
        if (p->pid == pid) {
            found = 1;
            break;
        }
    }
    if (!found)
        return -1;

    unsigned long irq;
    spin_lock_irqsave(&tsleep_lock, &irq);
    spin_lock(&p->lock);
    p->killed = 1;
    if (p->state == PROC_SLEEPING) {
        if (!list_empty(&p->tsleep_link)) {
            /* On tsleep_list — timer sleep path. */
            list_del_init(&p->tsleep_link);
        } else {
            /* On a wait queue (wq_sleep path).
             * TODO(Phase 9): should hold wq->lock for list_del, but we
             * don't know which queue p is on. Safe on single-hart. */
            list_del(&p->wait_link);
        }
        p->state = PROC_RUNNABLE;
        run_queue_add(p);
    }
    spin_unlock(&p->lock);
    spin_unlock_irqrestore(&tsleep_lock, irq);
    return 0;
}

/* --- Process creation --- */

/*
 * kthread_start — called at the beginning of every kernel thread.
 *
 * The scheduler holds p->lock when it switches to a new process (golden rule).
 * A brand-new thread has never called yield(), so it has no matching
 * spin_unlock. This function provides that, and enables interrupts.
 *
 * Must be the first thing every kernel thread function calls.
 */
void
kthread_start(void) {
    spin_unlock(&this_proc()->lock);
    intr_on();
}

/*
 * proc_create — shared base for all process allocation.
 *
 * Allocates and zeroes a proc struct, assigns a PID, allocates a kernel
 * stack, initializes synchronization primitives and list heads, and links
 * the proc into init_proc's children (default parent).
 *
 * Does NOT add to the run queue, all_procs, or pid_table — callers do
 * that after finishing their type-specific setup.
 *
 * fn: the function swtch will "ret" into on first dispatch.
 *   - kernel threads: the thread function (e.g. idle_thread, init_start)
 *   - user procs: user_proc_start (releases p->lock, calls user_trap_ret)
 *
 * Returns NULL on allocation failure (caller decides whether to panic or
 * propagate the error gracefully).
 */
static struct proc *
proc_create(void (*fn)(void), const char *name) {
    struct proc *p = kmalloc(sizeof(struct proc));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(struct proc));

    /* Identity */
    p->pid = alloc_pid();
    for (int i = 0; i < PROC_NAME_LEN - 1 && name[i]; i++)
        p->name[i] = name[i];

    /* Kernel stack */
    p->kstack = (uint64)kalloc();
    if (!p->kstack) {
        kmfree(p);
        return NULL;
    }

    /* First dispatch: swtch restores this context and "ret"s into fn. */
    p->context.ra = (uint64)fn;
    p->context.sp = p->kstack + PG_SIZE;

    spin_init(&p->lock, name);
    wq_init(&p->child_wq, name);
    INIT_LIST_HEAD(&p->children);
    INIT_LIST_HEAD(&p->vma_list);
    INIT_LIST_HEAD(&p->wait_link);
    INIT_LIST_HEAD(&p->tsleep_link);

    /* Parent-child linkage (PID 0 and 1 have no parent) */
    if (init_proc) {
        p->parent = init_proc;
        list_add_tail(&p->sibling, &init_proc->children);
    }

    return p;
}

/*
 * proc_create_kernel — create and publish a kernel thread.
 *
 * Calls proc_create(fn, name) for the base allocation, then marks the
 * process RUNNABLE and adds it to the run queue, all_procs list, and PID
 * hash table. On first dispatch, swtch "returns" into fn; fn must call
 * kthread_start() as its first action (releases p->lock + enables irqs).
 *
 * Panics on OOM — kernel threads are created during boot where failure
 * is unrecoverable.
 */
struct proc *
proc_create_kernel(void (*fn)(void), const char *name) {
    struct proc *p = proc_create(fn, name);
    if (!p)
        panic("proc_create_kernel: out of memory");

    /* Add to global structures */
    p->state = PROC_RUNNABLE;
    list_add_tail(&p->all_list, &all_procs);
    run_queue_add(p);
    hash_add(pid_table, &p->pid_link, PID_HASH_BITS, hash_int(p->pid));

    return p;
}

/* --- Initialization --- */

/*
 * proc_init — initialize the process subsystem.
 * Called once from kmain before any process is created.
 */
void
proc_init(void) {
    hash_init(pid_table, PID_HASH_BITS);
    memset(&cpus[0], 0, sizeof(struct cpu));
    spin_init(&wait_lock, "wait_lock");
    spin_init(&run_queue_lock, "run_queue_lock");
    spin_init(&pid_lock, "pid_lock");
    spin_init(&tsleep_lock, "tsleep_lock");
}

/* --- Kernel threads --- */

void
idle_thread(void) {
    kthread_start();
    for (;;) {
        asm volatile("wfi");
        yield();
    }
}

/* Defined in trap.c */
void user_trap(void);
void user_trap_ret(void);

/*
 * init_start — PID 1 kernel thread that execs into user-mode init.
 *
 * Created as a kernel thread so the scheduler is already running when
 * exec is called. This matters in Phase 7 when exec reads from disk
 * (needs interrupts + sleep). For now the binary is embedded in memory,
 * but the pattern is future-proof.
 *
 * After exec succeeds, this kernel thread is gone — replaced by the
 * user init program. If exec fails, panic (no init = no OS).
 */
static void
init_start(void) {
    kthread_start();
    if (proc_exec("init", NULL) < 0)
        panic("init_start: exec init failed");
    user_trap_ret(); /* enter user mode — never returns */
}

/*
 * proc_bootstrap — create bootstrap processes.
 *
 * PID 0: idle kernel thread (wfi loop).
 * PID 1: kernel thread that immediately execs into user init.
 */
void
proc_bootstrap(void) {
    proc_create_kernel(idle_thread, "idle");            /* PID 0 */
    init_proc = proc_create_kernel(init_start, "init"); /* PID 1 */
}

/* --- Timer-based sleep (Round 6-3) --- */

/*
 * wake_expired_sleepers — wake all procs whose wake_time has passed.
 *
 * Called from the timer tick handler in trap.c (both kernel_trap and
 * user_trap paths). Walks the sorted tsleep_list from front (earliest
 * deadline first), wakes expired procs, stops at the first non-expired
 * entry.
 *
 * Precondition: called from the timer interrupt handler, so interrupts are
 * already off — hence plain spin_lock on tsleep_lock (no irqsave needed).
 *
 * Lock ordering: tsleep_lock → p->lock (same as sys_sleep and proc_kill).
 *
 * Note: a proc may also be removed from tsleep_list by proc_kill (which
 * checks !list_empty(&p->tsleep_link) to distinguish timer-sleepers from
 * wait-queue-sleepers). Both paths hold tsleep_lock for list removal and
 * use list_del_init to reset the node to self-pointing (the "not on any
 * list" state).
 */
void
wake_expired_sleepers(void) {
    spin_lock(&tsleep_lock);

    uint64 now = read_time();
    struct proc *pos, *tmp; /* _safe: we list_del_init each node mid-loop */
    list_for_each_entry_safe(pos, tmp, &tsleep_list, tsleep_link) {
        if (now < pos->wake_time)
            break; /* list is sorted — all later entries are still in the future */
        list_del_init(&pos->tsleep_link); /* off tsleep_list; node now self-pointing */
        spin_lock(&pos->lock);
        pos->state = PROC_RUNNABLE;
        run_queue_add(pos);
        spin_unlock(&pos->lock);
    }

    spin_unlock(&tsleep_lock);
}

/* --- User process creation (Round 6-3) --- */

/*
 * user_proc_start — First-time entry stub for user processes.
 *
 * Analogous to kthread_start() for kernel threads: releases p->lock
 * (held across swtch by the scheduler, per the golden rule) then
 * enters user mode. We can't use kthread_start() because it calls
 * intr_on() — user processes instead call user_trap_ret() which
 * manages interrupts and stvec itself.
 *
 * context.ra = user_proc_start for new user processes.
 */
void
user_proc_start(void) {
    spin_unlock(&this_proc()->lock);
    user_trap_ret();
}

/*
 * proc_create_user — allocate a user process with an empty address space.
 *
 * Calls proc_create with user_proc_start as the entry stub (on first
 * dispatch, swtch "returns" into user_proc_start which releases p->lock
 * and calls user_trap_ret to drop into U-mode). Then allocates a
 * trapframe and user page table (trampoline + trapframe mapped; no user
 * pages yet). The caller (fork or exec) fills in the address space.
 *
 * Returns NULL on allocation failure (caller must handle it).
 *
 * See Lecture 6-3, Part 9.
 */
struct proc *
proc_create_user(void) {
    struct proc *p = proc_create(user_proc_start, "");
    if (!p)
        return NULL;
    p->trapframe = kalloc();
    if (!p->trapframe) {
        list_del(&p->sibling); /* undo proc_create's link to init_proc */
        kfree((void *)p->kstack);
        kmfree(p);
        return NULL;
    }
    memset(p->trapframe, 0, PG_SIZE);
    p->pagetable = proc_pagetable(p); /* maps trampoline + trapframe */
    if (!p->pagetable) {
        list_del(&p->sibling);
        kfree(p->trapframe);
        kfree((void *)p->kstack);
        kmfree(p);
        return NULL;
    }
    return p;
}

/*
 * proc_fork — create a child that is a copy of the calling process.
 *
 * Deep-copies the parent's address space (every VMA and its pages) and
 * its user register state, so the child resumes at the same instruction
 * (just after the fork ecall) with an identical view of memory — but as
 * an independent process with its own page table and kernel stack.
 *
 * The one observable difference is the return value, the classic fork
 * contract: the parent gets the child's PID (this function's return,
 * written to the parent's a0 by the syscall dispatcher); the child gets 0
 * (written directly into the child's trapframe->a0 below).
 *
 * Returns child PID on success, -1 on failure (no child created).
 *
 * See Lecture 6-3, Part 3.
 */
int
proc_fork(void) {
    struct proc *parent = this_proc();

    /* Allocate the child shell: proc struct, kstack, trapframe, and a fresh
     * page table (with trampoline + trapframe mapped, but no user pages). */
    struct proc *child = proc_create_user();
    if (!child)
        return -1;

    /* COW fork: see vma_dup_all in vma.c for the TODO. */
    if (vma_dup_all(child, parent) < 0) {
        /* Tear down the half-built child. proc_create linked its sibling
         * onto init_proc->children, so unlink before freeing. */
        list_del(&child->sibling);
        proc_free_pagetable(child->pagetable);
        kfree(child->trapframe);
        kfree((void *)child->kstack);
        kmfree(child);
        return -1;
    }

    /* Copy the parent's saved user registers (epc, sp, all GPRs) so the
     * child resumes exactly where the parent was in user mode. */
    memcpy(child->trapframe, parent->trapframe, sizeof(struct trapframe));
    child->trapframe->a0 = 0; /* fork() returns 0 in the child */

    /* Overwrite the kernel-bootstrap fields with the CHILD's own values —
     * copying the parent's would point the child at the parent's kstack. */
    child->trapframe->kernel_sp = child->kstack + PG_SIZE;
    child->trapframe->kernel_satp = csrr(satp);
    child->trapframe->user_trap = (uint64)user_trap;
    child->trapframe->hartid = 0;

    memcpy(child->name, parent->name, sizeof(child->name));

    /* Reparent: proc_create defaulted the child under init; move it under
     * the real parent. Under wait_lock to serialize with exit/wait. */
    unsigned long irq;
    spin_lock_irqsave(&wait_lock, &irq);
    list_del(&child->sibling);
    child->parent = parent;
    list_add_tail(&child->sibling, &parent->children);
    spin_unlock_irqrestore(&wait_lock, irq);

    /* Publish the child: make it schedulable and globally visible.
     *
     * Interrupts must be OFF here. sys_fork runs with interrupts ON (the
     * syscall path calls intr_on), and run_queue_add takes run_queue_lock
     * with a plain spin_lock. If a timer fired while we held that lock, its
     * handler (wake_expired_sleepers -> run_queue_add) would spin on the
     * same lock forever — single-hart self-deadlock. Disabling interrupts
     * across the publish block closes that window. (all_procs and pid_table
     * have no dedicated lock yet; they are only touched from syscall context
     * today, but keeping them inside the irq-off region is also correct and
     * is where a Phase 9 lock would go.) */
    unsigned long pirq = intr_get();
    intr_off();
    child->state = PROC_RUNNABLE;
    list_add_tail(&child->all_list, &all_procs);
    run_queue_add(child);
    hash_add(pid_table, &child->pid_link, PID_HASH_BITS, hash_int(child->pid));
    if (pirq)
        intr_on();

    return child->pid; /* parent's a0 (the child's PID) */
}

/*
 * proc_sbrk — grow or shrink the heap by n bytes.
 *
 * Returns the OLD break address on success, -1 on failure.
 * The break is always page-aligned.
 *
 * See Lecture 6-4, Part 2.
 */
int64
proc_sbrk(int64 n) {
    struct proc *p = this_proc();

    struct vma *v = vma_find_by_flags(p, VMA_HEAP);
    if (!v)
        return -1;

    uint64 old_end = v->end;

    if (n > 0) {
        /* Grow: extend VMA, don't allocate pages (demand paging handles it).
         * Page-aligned: sbrk(1) rounds up to a full page. Callers (malloc)
         * handle sub-page granularity internally. */
        uint64 new_end = PG_ROUND_UP(old_end + n);
        if (new_end > HEAP_MAX)
            return -1;
        v->end = new_end;
    } else if (n < 0) {
        /* Shrink: round down, unmap released pages, free backing memory.
         * Page-aligned: sbrk(-1) reclaims the entire last page. This is
         * correct — the break is page-granular, and user-space malloc never
         * shrinks by sub-page amounts. */
        if ((uint64)(-n) > (old_end - v->start))
            return -1;
        uint64 new_end = PG_ROUND_DOWN(old_end + n);
        if (new_end < v->start)
            new_end = v->start;

        for (uint64 va = new_end; va < old_end; va += PG_SIZE) {
            pte_t *pte = walk(p->pagetable, va, 0);
            /* Skip lazy pages that were never faulted in (no PTE to free) */
            if (!pte || !(*pte & PTE_V))
                continue;
            page_put((void *)pte_to_pa(*pte));
            *pte = 0;
        }
        sfence_vma();
        v->end = new_end;
    }
    /* n == 0: query — just return current break */

    return old_end;
}
