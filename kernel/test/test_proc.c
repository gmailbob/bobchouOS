/*
 * test_proc.c — Tests for process lifecycle (Round 5-2 additions).
 *
 * Full lifecycle tests (exit→zombie→wait reaps) require the scheduler
 * and are tested via `make run`. Unit tests here cover what can be
 * verified without scheduling: creation, parent-child linkage, PID
 * allocation, and kill on invalid PID.
 */

#include "test/test.h"
#include "proc.h"
#include "spinlock.h"
#include "list.h"

extern struct proc *init_proc;

static void
dummy_fn(void) {
}

void
test_proc(void) {
    kprintf("[test_proc]\n");

    /* proc_init initializes global locks */
    TEST_ASSERT(spin_holding(&wait_lock) == 0, "wait_lock initialized unlocked");

    /* proc_create_kernel: basic fields */
    struct proc *p = proc_create_kernel(dummy_fn, "test_p");
    TEST_ASSERT(p != 0, "proc_create_kernel: returns non-NULL");
    TEST_ASSERT(p->pid >= 0, "proc_create_kernel: PID >= 0");
    TEST_ASSERT(p->state == PROC_RUNNABLE, "proc_create_kernel: state RUNNABLE");
    TEST_ASSERT(p->kstack != 0, "proc_create_kernel: kstack allocated");
    TEST_ASSERT(p->context.ra == (uint64)dummy_fn, "proc_create_kernel: ra = fn");
    TEST_ASSERT(p->context.sp == p->kstack + 4096, "proc_create_kernel: sp = top of kstack");
    TEST_ASSERT(p->name[0] == 't', "proc_create_kernel: name copied");
    TEST_ASSERT(p->killed == 0, "proc_create_kernel: killed = 0");
    TEST_ASSERT(p->exit_status == 0, "proc_create_kernel: exit_status = 0");

    /* New 5-2 fields initialized */
    TEST_ASSERT(p->lock.locked == 0, "proc_create_kernel: lock initialized");
    TEST_ASSERT(list_empty(&p->children), "proc_create_kernel: children list empty");
    TEST_ASSERT(list_empty(&p->child_wq.head), "proc_create_kernel: child_wq empty");

    /* Parent-child linkage */
    TEST_ASSERT(p->parent == init_proc, "proc_create_kernel: parent = init_proc");
    if (init_proc) {
        int found = 0;
        struct proc *child;
        list_for_each_entry(child, &init_proc->children, sibling) {
            if (child == p) {
                found = 1;
                break;
            }
        }
        TEST_ASSERT(found, "proc_create_kernel: p in init_proc's children");
    }

    /* PID allocation is monotonically increasing */
    struct proc *p2 = proc_create_kernel(dummy_fn, "test_p2");
    TEST_ASSERT(p2->pid > p->pid, "alloc_pid: monotonically increasing");

    /* proc_kill on invalid PID returns -1 */
    TEST_ASSERT(proc_kill(99999) == -1, "proc_kill: invalid PID returns -1");
}
