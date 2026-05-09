/*
 * proc.h — Process management structures and declarations.
 *
 * See Lecture 5-1 for design rationale.
 */

#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "list.h"

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
    /* --- Identity --- */
    int pid;
    char name[PROC_NAME_LEN];
    enum proc_state state;

    /* --- Scheduling (embedded list nodes) --- */
    struct list_head all_list;
    struct list_head run_list;
    struct list_head pid_link;

    /* --- Family (Round 5-3) --- */
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;

    /* --- Execution state --- */
    struct context context;
    uint64 kstack;

    /* --- Address space (Round 5-2) --- */
    pagetable_t pagetable;
    struct trapframe *trapframe;
    uint64 sz;

    /* --- Exit (Round 5-3) --- */
    int exit_status;
};

/* --- struct cpu --- */

struct cpu {
    struct proc *proc;
    struct context scheduler;
    int noff;
    int need_resched;
};

/* --- Function declarations --- */

void proc_init(void);
struct proc *proc_create_kernel(void (*fn)(void), const char *name);
void scheduler(void);
void yield(void);
struct cpu *this_cpu(void);
struct proc *this_proc(void);

/* swtch is in swtch.S */
extern void swtch(struct context *old, struct context *new);

/* Kernel threads (defined in proc.c). */
void idle_thread(void);
void worker(void);

#endif /* PROC_H */
