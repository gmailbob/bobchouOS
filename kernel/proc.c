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
static int next_pid = 1;

/* --- PID allocation --- */

static int
alloc_pid(void) {
    return next_pid++;
}

/* --- Per-CPU accessors --- */

struct cpu *
this_cpu(void) {
    /* TODO: return &cpus[0].
     * (Single hart — later this reads tp to index cpus[].) */
    return 0;
}

struct proc *
this_proc(void) {
    /* TODO: return this_cpu()->proc.
     * Panic if NULL (called from context where a process must be running). */
    return 0;
}

/* --- Scheduler --- */

/*
 * pick_next — remove and return the first process from the run queue.
 * With the idle thread always present, this should never return NULL.
 */
static struct proc *
pick_next(void) {
    /* TODO: if run queue is empty, return NULL (shouldn't happen with idle).
     * Otherwise: get the first entry via list_first_entry, remove it from
     * the run queue with list_del, and return the proc. */
    return 0;
}

/*
 * scheduler — the main scheduling loop. Never returns.
 * Called as the last thing kmain() does. The boot stack becomes the
 * scheduler's permanent stack.
 */
void
scheduler(void) {
    /* TODO:
     * struct cpu *c = this_cpu();
     * Loop forever:
     *   1. p = pick_next()
     *   2. p->state = PROC_RUNNING
     *   3. c->proc = p
     *   4. sbi_set_timer(read_mtime() + TIMER_INTERVAL) — arm timeslice
     *   5. swtch(&c->scheduler, &p->context)
     *   6. (we return here when process yields/is preempted)
     *   7. c->proc = NULL
     */
}

/*
 * yield — voluntarily give up the CPU.
 * Marks current process RUNNABLE, pushes it to the run queue tail,
 * and switches back to the scheduler.
 */
void
yield(void) {
    /* TODO:
     * 1. p = this_proc()
     * 2. p->state = PROC_RUNNABLE
     * 3. list_add_tail(&p->run_list, &run_queue)
     * 4. swtch(&p->context, &this_cpu()->scheduler)
     */
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
    /* TODO:
     * 1. p = kmalloc(sizeof(struct proc))
     * 2. memset p to zero
     * 3. p->pid = alloc_pid()  (except idle: set p->pid = 0 directly)
     * 4. Copy name into p->name
     * 5. p->kstack = (uint64)kalloc()  — one page for kernel stack
     * 6. Set up context:
     *      memset(&p->context, 0, sizeof(struct context))
     *      p->context.ra = (uint64)fn
     *      p->context.sp = p->kstack + PG_SIZE  (top of stack, grows down)
     * 7. p->state = PROC_RUNNABLE
     * 8. INIT_LIST_HEAD(&p->children)
     * 9. list_add_tail(&p->all_list, &all_procs)
     * 10. list_add_tail(&p->run_list, &run_queue)
     * 11. hash_add(pid_table, &p->pid_link, PID_HASH_BITS, hash_int(p->pid))
     * 12. return p
     */
    return 0;
}

/* --- Initialization --- */

/*
 * proc_init — initialize the process subsystem.
 * Called once from kmain before any process is created.
 */
void
proc_init(void) {
    /* TODO:
     * 1. hash_init(pid_table, PID_HASH_BITS)
     * 2. Initialize cpus[0] (memset to zero is fine)
     * (run_queue and all_procs are statically initialized via LIST_HEAD)
     */
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
        if (count % 3000000 == 0)
            yield();
        if (count >= 10000000)
            count = 0;
    }
}
