/*
 * test_proc.c — Tests for process creation and fields.
 *
 * Covers proc_create_kernel (Phase 5) and proc_create_user (Phase 6).
 * Full lifecycle tests (exit→zombie→wait, fork, exec) require the
 * scheduler and are tested via `make run`.
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

    /* 5-2 fields initialized */
    TEST_ASSERT(p->lock.locked == 0, "proc_create_kernel: lock initialized");
    TEST_ASSERT(list_empty(&p->children), "proc_create_kernel: children list empty");
    TEST_ASSERT(list_empty(&p->child_wq.head), "proc_create_kernel: child_wq empty");

    /* 6-3 fields initialized */
    TEST_ASSERT(list_empty(&p->vma_list), "proc_create_kernel: vma_list empty");
    TEST_ASSERT(list_empty(&p->sleep_link), "proc_create_kernel: sleep_link self-pointing");
    TEST_ASSERT(p->wake_time == 0, "proc_create_kernel: wake_time = 0");
    TEST_ASSERT(p->pagetable == 0, "proc_create_kernel: no pagetable (kernel thread)");
    TEST_ASSERT(p->trapframe == 0, "proc_create_kernel: no trapframe (kernel thread)");

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

    /* proc_create_user: has trapframe + pagetable + user_proc_start */
    struct proc *up = proc_create_user();
    TEST_ASSERT(up != 0, "proc_create_user: returns non-NULL");
    TEST_ASSERT(up->trapframe != 0, "proc_create_user: trapframe allocated");
    TEST_ASSERT(up->pagetable != 0, "proc_create_user: pagetable allocated");
    TEST_ASSERT(list_empty(&up->vma_list), "proc_create_user: vma_list empty (no user pages yet)");
    extern void user_proc_start(void);
    TEST_ASSERT(up->context.ra == (uint64)user_proc_start,
                "proc_create_user: ra = user_proc_start");
    TEST_ASSERT(up->context.sp == up->kstack + 4096, "proc_create_user: sp = top of kstack");
}
