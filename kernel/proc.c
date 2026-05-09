/*
 * proc.c — Process management for bobchouOS.
 *
 * Implements process creation, the scheduler loop, yield, and
 * per-CPU state. Uses kmalloc for proc allocation, list_head for
 * the run queue and all-procs list, and a hash table for PID lookup.
 *
 * See Lecture 5-1.
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

/* --- Interrupt enable/disable helpers --- */

static inline void
intr_on(void) {
    csrw(sstatus, csrr(sstatus) | SSTATUS_SIE);
}

static inline void
intr_off(void) {
    csrw(sstatus, csrr(sstatus) & ~SSTATUS_SIE);
}

/* --- PID allocation --- */

static int
alloc_pid(void) {
    return next_pid++;
}

/* --- Per-CPU accessors --- */

struct cpu *
this_cpu(void) {
    return &cpus[0]; /* single hart; later reads tp to index cpus[] */
}

struct proc *
this_proc(void) {
    struct proc *p = this_cpu()->proc;
    if (p)
        return p;
    panic("this_proc: no current process");
}

/* --- Scheduler --- */

/*
 * pick_next — pop the first process from the run queue.
 * With idle always present, this should never return NULL.
 */
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
 * Called as the last thing kmain() does; the boot stack becomes
 * the scheduler's permanent stack.
 */
void
scheduler(void) {
    struct cpu *c = this_cpu();
    for (;;) {
        struct proc *p = pick_next();
        p->state = PROC_RUNNING;
        c->proc = p;
        sbi_set_timer(read_mtime() + TIMER_INTERVAL);
        swtch(&c->scheduler, &p->context);
        c->proc = NULL;
    }
}

/*
 * yield — voluntarily give up the CPU.
 * Marks current process RUNNABLE, pushes to run queue tail,
 * and switches back to the scheduler.
 */
void
yield(void) {
    struct proc *p = this_proc();
    p->state = PROC_RUNNABLE;
    list_add_tail(&p->run_list, &run_queue);
    swtch(&p->context, &this_cpu()->scheduler);
}

/* --- Process creation --- */

/*
 * proc_create_kernel — create a new kernel thread.
 *
 * Allocates proc via kmalloc, assigns PID, allocates kernel stack,
 * sets up context so swtch "returns" into fn, and adds proc to
 * run queue, all-procs list, and PID hash table.
 */
struct proc *
proc_create_kernel(void (*fn)(void), const char *name) {
    struct proc *p = kmalloc(sizeof(struct proc));
    memset(p, 0, sizeof(struct proc)); /* kmalloc does not zero */

    p->pid = alloc_pid();
    for (int i = 0; i < PROC_NAME_LEN - 1 && name[i]; i++)
        p->name[i] = name[i]; /* null-terminated: memset zeroed the rest */
    p->kstack = (uint64)kalloc();
    if (!p->kstack)
        panic("proc_create_kernel: kalloc failed");
    p->context.ra = (uint64)fn;
    p->context.sp = p->kstack + PG_SIZE; /* top of stack (grows down) */
    p->state = PROC_RUNNABLE;
    list_add_tail(&p->all_list, &all_procs);
    list_add_tail(&p->run_list, &run_queue);
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
}

/* --- Idle thread (PID 0) --- */

void
idle_thread(void) {
    intr_on(); /* new threads start with SIE=0; enable for timer preemption */
    for (;;) {
        asm volatile("wfi");
        yield();
    }
}

/* --- Worker thread (for testing) --- */

void
worker(void) {
    intr_on();
    int count = 0;
    for (;;) {
        count++;
        if (count % 1000000 == 0) {
            intr_off(); /* kprintf is not reentrant */
            kprintf("[%s] pid=%d count=%d\n",
                    this_proc()->name, this_proc()->pid, count / 1000000);
            intr_on();
        }
        if (count >= 100000000)
            count = 0;
    }
}
