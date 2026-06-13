/*
 * proc.h — Process management structures and declarations.
 *
 * See Lectures 5-1/5-2 for scheduling and context switch design,
 * and Lecture 6-3 for fork/exec/wait/kill/sleep and VMA integration.
 */

#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "list.h"
#include "spinlock.h"
#include "vm.h"
#include "vma.h"
#include "wait_queue.h"

/* Number of PID hash table buckets (2^PID_HASH_BITS = 64). */
#define PID_HASH_BITS 6

/* Maximum process name length (including null terminator). */
#define PROC_NAME_LEN 16

/* --- Process states --- */

enum proc_state {
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_ZOMBIE,
};

/* --- struct context ---
 *
 * Saved callee-saved registers for swtch(). When a process is not
 * running, its context holds the register state needed to resume.
 * ra is the address to jump to on resume (not a standard callee-save,
 * but stored for the same reason — see Lecture 5-1, Part 2).
 *
 * Field order must match the sd/ld offsets in swtch.S.
 */
struct context {
    uint64 ra;
    uint64 sp;
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

/* Forward declarations for types not yet needed. */
struct trapframe;

/* --- struct proc --- */

struct proc {
    struct spinlock lock; /* protects: state, killed, exit_status */

    /* --- Identity --- */
    int pid;
    char name[PROC_NAME_LEN];
    enum proc_state state;

    /* --- Scheduling (embedded list nodes) --- */
    struct list_head all_list; /* node in global all_procs list */
    struct list_head run_list; /* node in run queue (only when RUNNABLE) */
    struct list_head pid_link; /* node in PID hash table bucket */

    /* --- Family --- */
    struct proc *parent;
    struct list_head children;  /* head of this proc's children list */
    struct list_head sibling;   /* node in parent->children list */
    struct wait_queue child_wq; /* this proc sleeps here when calling wait() */

    /* --- Execution state --- */
    struct context context; /* saved callee-regs + ra for swtch */
    uint64 kstack;          /* base address of kernel stack page */

    /* --- Address space (Phase 6) --- */
    pte_t *pagetable;
    struct trapframe *trapframe;
    struct list_head vma_list; /* sorted list of VMAs (replaces sz) */

    /* --- Lifecycle --- */
    int killed;                 /* set by kill(), checked in kernel_trap_ret and sleep loops */
    int exit_status;            /* passed to exit(), read by parent in wait() */
    struct list_head wait_link; /* node on a wait queue (only when SLEEPING) */

    /* --- Sleep (sys_sleep) --- */
    uint64 wake_time;            /* mtime deadline for timed sleep (0 = not sleeping) */
    struct list_head sleep_link; /* node in global sleep_list */
};

/* --- struct cpu --- */

struct cpu {
    struct proc *proc;
    struct context scheduler;
    int need_resched;
};

/* --- Function declarations --- */

void proc_init(void);
void proc_bootstrap(void);
void kthread_start(void);
struct proc *proc_create_kernel(void (*fn)(void), const char *name);
struct proc *proc_create_user(void); /* allocate proc + pagetable + trapframe (no VMAs yet) */
int proc_fork(void);
int64 proc_sbrk(int64 n);
int proc_exec(const char *path, char **argv);
void wake_expired_sleepers(void);
void scheduler(void);
void run_queue_add(struct proc *p);
void yield(void);
void sched(void);
void proc_exit(int status);
int proc_wait(int *status);
int proc_kill(int pid);
struct cpu *this_cpu(void);
struct proc *this_proc(void);

/* swtch is in swtch.S */
extern void swtch(struct context *old, struct context *new);

/* Global locks and lists (defined in proc.c). */
extern struct spinlock wait_lock;
extern struct spinlock sleep_lock;
extern struct list_head sleep_list;

#endif /* PROC_H */
