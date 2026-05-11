/*
 * proc.h — Process management structures and declarations.
 *
 * See Lectures 5-1 and 5-2 for design rationale.
 */

#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "list.h"
#include "spinlock.h"
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
typedef uint64 *pagetable_t;
struct trapframe;

/* --- struct proc --- */

struct proc {
    struct spinlock lock; /* protects: state, killed, exit_status */

    /* --- Identity --- */
    int pid;
    char name[PROC_NAME_LEN];
    enum proc_state state;

    /* --- Scheduling (embedded list nodes) --- */
    struct list_head all_list;
    struct list_head run_list;
    struct list_head pid_link;

    /* --- Family --- */
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;
    struct wait_queue child_wq; /* this proc sleeps here when calling wait() */

    /* --- Execution state --- */
    struct context context;
    uint64 kstack;

    /* --- Address space (Phase 6) --- */
    pagetable_t pagetable;
    struct trapframe *trapframe;
    uint64 sz;

    /* --- Lifecycle --- */
    int killed;
    int exit_status;
    struct list_head wait_link; /* for sleeping on a wait queue */
};

/* --- struct cpu --- */

struct cpu {
    struct proc *proc;
    struct context scheduler;
    int need_resched;
};

/* --- Function declarations --- */

void proc_init(void);
struct proc *proc_create_kernel(void (*fn)(void), const char *name);
void scheduler(void);
void yield(void);
void sched(void);
void exit(int status);
int wait(int *status);
int kill(int pid);
struct cpu *this_cpu(void);
struct proc *this_proc(void);

/* swtch is in swtch.S */
extern void swtch(struct context *old, struct context *new);

/* Global locks (defined in proc.c). */
extern struct spinlock wait_lock;
extern struct spinlock run_queue_lock;
extern struct spinlock pid_lock;
extern struct proc *init_proc;

/* Kernel threads (defined in proc.c). */
void idle_thread(void);
void init_thread(void);
void worker(void);

#endif /* PROC_H */
