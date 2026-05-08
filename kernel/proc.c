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

/* --- PID allocation --- */

static int
alloc_pid(void) {
    return next_pid++;
}

/* --- Per-CPU accessors --- */

struct cpu *
this_cpu(void) {
    /* (Single hart — later this reads tp to index cpus[].) */
    return &cpus[0];
}

struct proc *
this_proc(void) {
    /* should be called from context where a process must be running. */
    if (this_cpu()->proc)
        return this_cpu()->proc;
    panic("this_proc: NULL");
}

/* --- Scheduler --- */

/*
 * pick_next — remove and return the first process from the run queue.
 * With the idle thread always present, this should never return NULL.
 */
static struct proc *
pick_next(void) {
    if (list_empty(&run_queue))
        return NULL;

    struct proc *p = list_first_entry(&run_queue, struct proc, run_list);
    list_del(&p->run_list);
    return p;
}

/*
 * scheduler — the main scheduling loop. Never returns.
 * Called as the last thing kmain() does. The boot stack becomes the
 * scheduler's permanent stack.
 */
void
scheduler(void) {
    struct cpu *c = this_cpu();
    for (;;) {
        struct proc *p = pick_next();
        p->state = PROC_RUNNING;
        c->proc = p;
        kprintf("scheduler picked %s\n", p->name);
        sbi_set_timer(read_mtime() + TIMER_INTERVAL);
        kprintf("scheduler set timer %d\n", read_mtime() + TIMER_INTERVAL);
        swtch(&c->scheduler, &p->context); // when p yields, go to next line (instruction)
        c->proc = NULL;
    }
}

/*
 * yield — voluntarily give up the CPU.
 * Marks current process RUNNABLE, pushes it to the run queue tail,
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
 * Allocates a proc via kmalloc, assigns a PID, allocates a kernel stack,
 * sets up context so that swtch "returns" into fn, and adds the proc
 * to the run queue, all-procs list, and PID hash table.
 */
struct proc *
proc_create_kernel(void (*fn)(void), const char *name) {
    struct proc *p = kmalloc(sizeof(struct proc));
    memset(p, 0, sizeof(struct proc)); // in our design kmalloc is not zeroed

    p->pid = alloc_pid();
    memcpy(p->name, name, PROC_NAME_LEN);
    p->kstack = (uint64)kalloc();
    p->context.ra = (uint64)fn;
    p->context.sp = p->kstack + PG_SIZE; // top of stack, grows down
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

/* --- Idle thread --- */

/*
 * idle_thread — the idle process (PID 0). Runs when no other process
 * is runnable. Halts the hart until an interrupt arrives.
 */
void
idle_thread(void) {
    for (;;) {
        asm volatile("wfi");
        yield();
    }
}

/* --- Worker thread (for testing) --- */

void
worker(void) {
    int count = 0;
    for (;;) {
        count++;
        if (count % 1000000 == 0)
            kprintf("[%s] %d\n", this_proc()->name, count / 1000000);
        // if (count % 23000000 == 0) {
        //     kprintf("[%s] self-yield\n", this_proc()->name);
        //     yield();
        // }
        if (count >= 100000000)
            count = 0;
    }
}
