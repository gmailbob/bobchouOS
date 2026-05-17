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
#include "riscv.h"
#include "sbi.h"
#include "string.h"

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

/* --- PID allocation --- */

/*
 * alloc_pid — allocate the next PID.
 */
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

void
run_queue_add(struct proc *p) {
    spin_lock(&run_queue_lock);
    list_add_tail(&p->run_list, &run_queue);
    spin_unlock(&run_queue_lock);
}

/*
 * sched — switch from current process to scheduler.
 * Caller MUST hold p->lock (the golden rule).
 * Scheduler releases p->lock on the other side.
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
 * add locking per the golden rule:
 * - Release prev->lock after swtch returns
 * - Acquire run_queue_lock around pick_next
 * - Acquire p->lock before setting state and swtch
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
        // later p will release the lock, do work and then re-lock when crossing swtch
        swtch(&c->scheduler, &p->context);
        c->proc = NULL;
        spin_unlock(&p->lock);
    }
}

/*
 * yield — give up the CPU.
 * Called voluntarily by a process, or by ret_from_trap on timer preemption.
 *
 * add locking per the golden rule:
 * - Acquire run_queue_lock (plain — interrupts off from trap or caller)
 * - Acquire p->lock (plain)
 * - Set state RUNNABLE, add to run queue tail
 * - Release run_queue_lock
 * - Call sched() (p->lock crosses into scheduler)
 * - After sched returns: release p->lock
 *
 * Currently: unlocked version from Round 5-1.
 */
void
yield(void) {
    struct proc *p = this_proc();
    spin_lock(&p->lock);
    run_queue_add(p);
    p->state = PROC_RUNNABLE;
    swtch(&p->context, &this_cpu()->scheduler);
    spin_unlock(&p->lock);
}

/*
 * exit — terminate the current process.
 *
 * - Panic if init_proc is exiting
 * - Acquire wait_lock (irqsave — outermost)
 * - Reparent children to init_proc:
 *     list_for_each_entry over p->children, set child->parent = init_proc
 *     list_splice(&p->children, &init_proc->children) to move list
 * - wq_wake_one(&p->parent->child_wq) — wake parent
 * - Acquire p->lock (plain — inner)
 * - Set p->exit_status = status, p->state = PROC_ZOMBIE
 * - spin_unlock(&wait_lock) — release wait_lock (plain, not irqrestore,
 *   because we still hold p->lock and can't restore interrupts yet)
 * - sched() — never returns, p->lock released by scheduler
 */
void
exit(int status) {
    struct proc *p = this_proc();
    if (p->pid <= 1)
        panic("idel or init proc exit.");

    unsigned long irq; // dead data, use irq variant just to disable interrupt
    spin_lock_irqsave(&wait_lock, &irq);

    struct proc *child, *tmp;
    list_for_each_entry_safe(child, tmp, &p->children, sibling) {
        child->parent = init_proc;
        list_del(&child->sibling); // remove child from p->children
        list_add_tail(&child->sibling, &init_proc->children);
    }

    wq_wake_one(&p->parent->child_wq);
    spin_lock(&p->lock);
    p->exit_status = status;
    p->state = PROC_ZOMBIE;
    spin_unlock(&wait_lock);
    sched();
}

/*
 * wait — wait for a child to exit, reap it.
 *
 * Returns the child's PID (or -1 if no children).
 * Writes child's exit status to *status if non-NULL.
 *
 * TODO:
 * - Acquire wait_lock (irqsave — outermost)
 * - Loop:
 *   - Scan children list (list_for_each_entry over p->children, sibling)
 *   - For each child: acquire child->lock (plain), check if ZOMBIE
 *     - If ZOMBIE: save pid/status, release child->lock, freeproc(child),
 *       release wait_lock (irqrestore), return pid
 *     - If not: release child->lock, continue
 *   - If no children at all: release wait_lock (irqrestore), return -1
 *   - If children exist but no zombie: wq_sleep(&p->child_wq, &wait_lock)
 *
 * freeproc(child) should:
 * - list_del(&child->sibling)    — remove from parent's children
 * - list_del(&child->all_list)   — remove from global all_procs
 * - list_del(&child->pid_link)   — remove from PID hash table
 * - kfree((void *)child->kstack) — free kernel stack page
 * - kmfree(child)                — free proc struct
 */
int
wait(int *status) {
    struct proc *p = this_proc();

    unsigned long irq;
    spin_lock_irqsave(&wait_lock, &irq);

    if (list_empty(&p->children)) {
        spin_unlock_irqrestore(&wait_lock, irq);
        return -1;
    }

    struct proc *child;
    list_for_each_entry(child, &p->children, sibling) {
        spin_lock(&child->lock);
        if (child->state == PROC_ZOMBIE) {
            int pid = child->pid;
            *status = child->exit_status;
            list_del(&child->sibling);
            list_del(&child->all_list);
            list_del(&child->pid_link);
            kfree((void *)child->kstack);
            kmfree(child); // clock also recycled
            spin_unlock_irqrestore(&wait_lock, irq);
            return pid;
        }
        spin_unlock(&child->lock);
    }

    wq_sleep(&p->child_wq, &wait_lock);
    return 0;
}

/*
 * kill — mark a process for termination.
 *
 * Sets p->killed = 1. If the process is SLEEPING, wakes it
 * (moves to RUNNABLE + run queue) so it can notice the flag.
 *
 * TODO:
 * - Look up process by PID in pid_table using hash_for_each_entry
 * - If not found, return -1
 * - Acquire p->lock (irqsave — entry point)
 * - Set p->killed = 1
 * - If p->state == PROC_SLEEPING:
 *   - Remove from wait queue: list_del(&p->wait_link)
 *     (note: ideally under wq->lock, but we may not know which queue —
 *      for now, p->lock serialization is sufficient on single-hart)
 *   - Set state = PROC_RUNNABLE
 *   - Acquire run_queue_lock, list_add_tail to run queue, release
 * - Release p->lock (irqrestore)
 * - Return 0
 */
int
kill(int pid) {
    int bucket = hash_int(pid) & (HT_SIZE(PID_HASH_BITS) - 1);
    struct proc *p = NULL;
    list_for_each_entry(p, &pid_table[bucket], pid_link) {
        if (p->pid == pid)
            break;
    }
    if (!p)
        return -1;

    unsigned long irq;
    spin_lock_irqsave(&p->lock, &irq);
    p->killed = 1;
    if (p->state == PROC_SLEEPING) {
        list_del(&p->wait_link); // TODO: should lock wq
        p->state = PROC_RUNNABLE;
    }
    spin_unlock_irqrestore(&p->lock, irq);
    return 0;
}

/* --- Process creation --- */

/*
 * proc_create_kernel — create a new kernel thread.
 *
 * Allocates proc via kmalloc, assigns PID, allocates kernel stack,
 * sets up context so swtch "returns" into fn, and adds proc to
 * run queue, all-procs list, and PID hash table.
 *
 * Parent is set to init_proc (or NULL for idle/init themselves).
 *
 * TODO: add initialization for new fields:
 * - spin_init(&p->lock, name)
 * - wq_init(&p->child_wq, name)
 * - INIT_LIST_HEAD(&p->children)
 * - INIT_LIST_HEAD(&p->wait_link)
 * - Set p->parent and link into parent's children list
 * - Protect PID allocation and run queue with appropriate locks
 */
struct proc *
proc_create_kernel(void (*fn)(void), const char *name) {
    struct proc *p = kmalloc(sizeof(struct proc));
    memset(p, 0, sizeof(struct proc));

    unsigned long irq;
    spin_lock_irqsave(&pid_lock, &irq);
    p->pid = alloc_pid();
    spin_unlock_irqrestore(&pid_lock, irq);
    for (int i = 0; i < PROC_NAME_LEN - 1 && name[i]; i++)
        p->name[i] = name[i];

    p->kstack = (uint64)kalloc();
    if (!p->kstack)
        panic("proc_create_kernel: kalloc failed");

    p->context.ra = (uint64)fn;
    p->context.sp = p->kstack + PG_SIZE;
    p->state = PROC_RUNNABLE;

    spin_init(&p->lock, name);
    wq_init(&p->child_wq, name);
    INIT_LIST_HEAD(&p->children);
    INIT_LIST_HEAD(&p->wait_link);
    if (p->pid > 1) {
        p->parent = init_proc;
        list_add(&p->sibling, &init_proc->children);
    }

    list_add_tail(&p->all_list, &all_procs);
    run_queue_add(p);
    hash_add(pid_table, &p->pid_link, PID_HASH_BITS, hash_int(p->pid));

    return p;
}

/* --- Initialization --- */

/*
 * proc_init — initialize the process subsystem.
 *
 * TODO: initialize all global locks via spin_init:
 * - wait_lock, run_queue_lock, pid_lock
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
    intr_on();
    for (;;) {
        asm volatile("wfi");
        yield();
    }
}

/*
 * init_thread — the init process (PID 1).
 * Root of the process tree. Loops calling wait() to reap zombies.
 *
 * TODO:
 * - intr_on()
 * - Loop forever: call wait(&status)
 *   - If pid > 0: print "init: reaped pid %d (status %d)"
 *     (disable interrupts around kprintf)
 */
void
init_thread(void) {
    intr_on();
    int pid;
    int status;
    for (;;) {
        pid = wait(&status);
        if (pid)
            kprintf("init: reaped pid %d (status %d)", pid, status);
    }
}

void
worker(void) {
    intr_on();
    for (int i = 0; i < 5; i++) {
        intr_off();
        kprintf("[%s] count=%d\n", this_proc()->name, i);
        intr_on();
    }
    exit(0);
}

/*
 * proc_bootstrap — create the bootstrap kernel threads.
 *
 * In Phase 6, this becomes: create idle + a single init process that
 * calls exec("/init") to load the user-space init binary. Workers
 * will be spawned by user-space init via fork+exec instead.
 */
void
proc_bootstrap(void) {
    proc_create_kernel(idle_thread, "idle");             /* PID 0 */
    init_proc = proc_create_kernel(init_thread, "init"); /* PID 1 */
    proc_create_kernel(worker, "worker_a");              /* PID 2 */
    proc_create_kernel(worker, "worker_b");              /* PID 3 */
}
