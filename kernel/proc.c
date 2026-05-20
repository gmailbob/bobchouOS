/*
 * proc.c — Process management for bobchouOS.
 *
 * Implements process creation, scheduling, yield, sleep/wakeup,
 * exit, wait, kill, and per-CPU state.
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
struct proc *init_proc;

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
 * Precondition: caller holds p->lock (the golden rule).
 * Postcondition: scheduler releases p->lock on the other side.
 *                When sched returns, scheduler has re-acquired p->lock for us.
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
        sbi_set_timer(read_mtime() + TIMER_INTERVAL);

        /* p resumes here (kthread_start/yield) and releases p->lock we
         * just acquired. p runs freely, then re-acquires p->lock when it
         * calls sched() to yield back. In wq_sleep, p->lock is never
         * released by p itself — the scheduler does it below. */
        swtch(&c->scheduler, &p->context);

        /* p yielded back (holding p->lock from yield/exit/wq_sleep) */
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
    sched();                               /* p->lock crosses boundary */
    spin_unlock_irqrestore(&p->lock, irq); /* scheduler re-acquired it for us */
}

/*
 * exit — terminate the current process.
 *
 * Reparents children to init, wakes parent, marks ZOMBIE, yields forever.
 * Never returns.
 */
void
exit(int status) {
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
    kfree((void *)p->kstack);
    kmfree(p);
}

/*
 * wait — wait for a child to exit, reap it.
 *
 * Returns the child's PID (or -1 if no children exist).
 * Writes child's exit status to *status if non-NULL.
 *
 * Sleeps until a child exits if children exist but none are zombies yet.
 */
int
wait(int *status) {
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
         * exit instead of sleeping again. */
        if (p->killed) {
            spin_unlock_irqrestore(&wait_lock, irq);
            exit(-1);
        }

        /* Children exist but no zombie — sleep until one exits */
        wq_sleep(&p->child_wq, &wait_lock);
        /* wq_sleep releases wait_lock, sleeps, re-acquires it */
    }
}

/*
 * kill — mark a process for termination.
 *
 * Sets p->killed = 1. If SLEEPING, wakes it so it can notice the flag.
 * Returns 0 on success, -1 if PID not found.
 */
int
kill(int pid) {
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
    spin_lock_irqsave(&p->lock, &irq);
    p->killed = 1;
    if (p->state == PROC_SLEEPING) {
        /* TODO(Phase 9): should hold wq->lock for list_del, but we don't
         * know which queue p is on. Safe on single-hart (interrupts off). */
        list_del(&p->wait_link);
        p->state = PROC_RUNNABLE;
        run_queue_add(p);
    }
    spin_unlock_irqrestore(&p->lock, irq);
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
    struct proc *p = kmalloc(sizeof(struct proc));
    memset(p, 0, sizeof(struct proc));

    /* Identity */
    p->pid = alloc_pid();
    for (int i = 0; i < PROC_NAME_LEN - 1 && name[i]; i++)
        p->name[i] = name[i];

    /* Kernel stack */
    p->kstack = (uint64)kalloc();
    if (!p->kstack)
        panic("proc_create_kernel: kalloc failed");

    /* Context: swtch will "ret" into fn. fn must call kthread_start()
     * as its first action (releases p->lock + enables interrupts). */
    p->context.ra = (uint64)fn;
    p->context.sp = p->kstack + PG_SIZE;

    /* Initialize new 5-2 fields */
    spin_init(&p->lock, name);
    wq_init(&p->child_wq, name);
    INIT_LIST_HEAD(&p->children);
    INIT_LIST_HEAD(&p->wait_link);

    /* Parent-child linkage (PID 0 and 1 have no parent) */
    if (init_proc) {
        p->parent = init_proc;
        list_add_tail(&p->sibling, &init_proc->children);
    }

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
 * init_thread — the init process (PID 1).
 * Root of the process tree. Loops calling wait() to reap zombies forever.
 */
void
init_thread(void) {
    kthread_start();
    for (;;) {
        int status;
        int pid = wait(&status);
        if (pid > 0) {
            intr_off();
            kprintf("init: reaped pid %d (status %d)\n", pid, status);
            intr_on();
        }
    }
}

/*
 * proc_bootstrap — create the bootstrap kernel threads.
 *
 * In Phase 6, this becomes: create idle + a single init that calls
 * exec("/init") to load the user-space init binary.
 */
void
proc_bootstrap(void) {
    proc_create_kernel(idle_thread, "idle");             /* PID 0 */
    init_proc = proc_create_kernel(init_thread, "init"); /* PID 1 */
    proc_create_user_test();                             /* PID 2 — test user process */
}

/* --- User process creation (Round 6-1) --- */

/* Embedded test user binary (from user_test_bin.S via .incbin). */
extern char test_user_bin[];
extern char test_user_bin_end[];

/* user_trap_ret (defined in trap.c) — entry point for first trip to user mode. */
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
    /* TODO: Release p->lock (scheduler held it across swtch).
     * TODO: Call user_trap_ret() to enter user mode for the first time.
     */
}

/*
 * proc_create_user_test — Create the Round 6-1 test user process.
 *
 * Allocates a process, builds its user page table, maps a hardcoded
 * user program at VA 0x1000, and prepares it for first entry to user mode.
 *
 * See Lecture 6-1, Part 5.
 */
void
proc_create_user_test(void) {
    /* TODO: Allocate process struct (alloc_proc or equivalent).
     * TODO: Allocate trapframe page via kalloc().
     * TODO: Create user page table via proc_pagetable(p).
     * TODO: Allocate a page for user code, copy test_user_bin into it.
     * TODO: Map user code at VA 0x1000 (PTE_R | PTE_X | PTE_U).
     * TODO: Allocate user stack page, map at VA 0x3000 (PTE_R | PTE_W | PTE_U).
     * TODO: Initialize trapframe: epc = 0x1000, sp = 0x4000 (top of stack page).
     * TODO: Set p->sz = 0x4000.
     * TODO: Set p->context.ra = (uint64)user_proc_start (first-time entry stub).
     * TODO: Set p->context.sp = p->kstack + PG_SIZE (kernel stack top).
     * TODO: Mark RUNNABLE, add to run queue.
     */
    (void)test_user_bin;
    (void)test_user_bin_end;
}
