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
struct spinlock sleep_lock;
struct proc *init_proc;

LIST_HEAD(sleep_list); /* sorted by wake_time, earliest first */

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

        /* Arm timer for whichever comes first: quantum expiry or earliest sleeper.
         * This gives precise sleep wakeup rather than up-to-one-tick latency. */
        uint64 deadline = read_mtime() + TIMER_INTERVAL;
        spin_lock(&sleep_lock);
        if (!list_empty(&sleep_list)) {
            struct proc *sleeper = list_first_entry(&sleep_list, struct proc, sleep_link);
            if (sleeper->wake_time < deadline)
                deadline = sleeper->wake_time;
        }
        spin_unlock(&sleep_lock);
        sbi_set_timer(deadline);

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
 * free_proc — remove a zombie process from all data structures and free it.
 *
 * Precondition: caller holds wait_lock.
 */
static void
free_proc(struct proc *p) {
    list_del(&p->sibling);
    list_del(&p->all_list);
    list_del(&p->pid_link);
    /* free user resources */
    if (p->pagetable) {
        vma_free_all(p);
        proc_free_pagetable(p->pagetable);
        kfree(p->trapframe);
    }
    kfree((void *)p->kstack);
    kmfree(p);
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
 * Lock ordering: sleep_lock → p->lock (same as sys_sleep and
 * wake_expired_sleepers). We acquire sleep_lock first when the target
 * might be on the sleep_list.
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
    spin_lock_irqsave(&sleep_lock, &irq);
    spin_lock(&p->lock);
    p->killed = 1;
    if (p->state == PROC_SLEEPING) {
        if (!list_empty(&p->sleep_link)) {
            /* On sleep_list — timer sleep path. */
            list_del_init(&p->sleep_link);
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
    spin_unlock_irqrestore(&sleep_lock, irq);
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
static inline void
kthread_start(void) {
    spin_unlock(&this_proc()->lock);
    intr_on();
}

static struct proc *
proc_create(void (*fn)(void), const char *name) {
    struct proc *p = kmalloc(sizeof(struct proc));
    memset(p, 0, sizeof(struct proc));

    /* Identity */
    p->pid = alloc_pid();
    for (int i = 0; i < PROC_NAME_LEN - 1 && name[i]; i++)
        p->name[i] = name[i];

    /* Kernel stack */
    p->kstack = (uint64)kalloc();
    if (!p->kstack)
        panic("proc_create: kalloc failed");

    /* Context: swtch will "ret" into fn. fn must call kthread_start()
     * as its first action (releases p->lock + enables interrupts). */
    p->context.ra = (uint64)fn;
    p->context.sp = p->kstack + PG_SIZE;

    spin_init(&p->lock, name);
    wq_init(&p->child_wq, name);
    INIT_LIST_HEAD(&p->children);
    INIT_LIST_HEAD(&p->wait_link);
    INIT_LIST_HEAD(&p->sleep_link);

    /* Parent-child linkage (PID 0 and 1 have no parent) */
    if (init_proc) {
        p->parent = init_proc;
        list_add_tail(&p->sibling, &init_proc->children);
    }

    return p;
}

/*
 * proc_create_kernel — create a new kernel thread.
 *
 * Allocates proc, assigns PID, allocates kstack, sets up context so
 * swtch "returns" into user_proc_start (which releases p->lock, then calls fn).
 * Adds to run queue, all-procs, PID hash table.
 * Parent is init_proc for PID > 1 (NULL for idle/init themselves).
 */
struct proc *
proc_create_kernel(void (*fn)(void), const char *name) {
    struct proc *p = proc_create(fn, name);

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
    spin_init(&sleep_lock, "sleep_lock");
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
    proc_exec("init", NULL);
    panic("init_start: exec init failed");
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
 * user_trap paths). Walks the sorted sleep_list from front (earliest
 * deadline first), wakes expired procs, stops at the first non-expired
 * entry.
 *
 * Lock ordering: sleep_lock → p->lock (same as sys_sleep and proc_kill).
 *
 * Note: a proc may also be removed from sleep_list by proc_kill (which
 * checks !list_empty(&p->sleep_link) to distinguish timer-sleepers from
 * wait-queue-sleepers). Both paths hold sleep_lock for list removal and
 * use list_del_init to reset the node to self-pointing (the "not on any
 * list" state).
 */
void
wake_expired_sleepers(void) {
    spin_lock(&sleep_lock);

    uint64 now = read_mtime();
    struct proc *pos, *tmp;
    list_for_each_entry_safe(pos, tmp, &sleep_list, sleep_link) {
        if (now < pos->wake_time)
            break;
        list_del_init(&pos->sleep_link);
        spin_lock(&pos->lock);
        pos->state = PROC_RUNNABLE;
        run_queue_add(pos);
        spin_unlock(&pos->lock);
    }

    spin_unlock(&sleep_lock);
}

/* --- User process creation (Round 6-3) --- */

/* Defined in trap.c */
void user_trap(void);
void user_trap_ret(void);

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
static void
user_proc_start(void) {
    spin_unlock(&this_proc()->lock);
    user_trap_ret();
}

/*
 * proc_create_user — allocate a user process with an empty address space.
 *
 * Allocates proc struct, kernel stack, trapframe, and page table.
 * Returns the process ready for the caller to fill in (exec or fork).
 *
 * See Lecture 6-3, Part 9.
 */
struct proc *
proc_create_user(void) {
    struct proc *p = proc_create(NULL, "");
    // vmalist
    p->trapframe = kalloc();
    proc_pagetable(p);
    p->context.ra = (uint64)user_proc_start;
    return p;
}

/*
 * proc_fork — create a child process as a copy of the current process.
 *
 * Returns child PID to parent, sets child's trapframe->a0 = 0.
 * Returns -1 on failure.
 *
 * See Lecture 6-3, Part 3.
 */
int
proc_fork(void) {
    /* TODO(student): call proc_create_user, vma_dup_all, copy trapframe,
     * set child->trapframe->a0 = 0, set parent/child links,
     * mark RUNNABLE, add to scheduler. Return child pid. */
    return -1;
}
