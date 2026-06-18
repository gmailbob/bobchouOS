/*
 * test_proc.c — Integration tests for process lifecycle.
 *
 * Covers: proc_create_kernel, proc_create_user (field verification),
 * plus proc_exit/proc_wait, proc_kill, and yield (now possible with
 * a live scheduler).
 */

#include "test/test.h"
#include "proc.h"
#include "spinlock.h"
#include "list.h"
#include "wait_queue.h"

extern struct proc *init_proc;

static void
dummy_fn(void) {
    kthread_start();
    proc_exit(0);
}

static void
exit_42_thread(void) {
    kthread_start();
    proc_exit(42);
}

static volatile int yield_counter;

static void
yield_thread(void) {
    kthread_start();
    for (int i = 0; i < 5; i++) {
        __sync_fetch_and_add(&yield_counter, 1);
        yield();
    }
    proc_exit(0);
}

static struct wait_queue kill_wq;
static struct spinlock kill_lk;
static volatile int kill_cond;

static void
sleep_forever_thread(void) {
    kthread_start();

    unsigned long irq;
    spin_lock_irqsave(&kill_lk, &irq);
    while (!kill_cond) {
        wq_sleep(&kill_wq, &kill_lk);
        if (this_proc()->killed) {
            spin_unlock_irqrestore(&kill_lk, irq);
            proc_exit(-1);
        }
    }
    spin_unlock_irqrestore(&kill_lk, irq);
    proc_exit(0);
}

void
test_proc(void) {
    kprintf("[test_proc]\n");

    /* --- Creation: field verification --- */

    TEST_ASSERT(spin_holding(&wait_lock) == 0, "wait_lock initialized unlocked");

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
    TEST_ASSERT(p->lock.locked == 0, "proc_create_kernel: lock initialized");
    TEST_ASSERT(list_empty(&p->children), "proc_create_kernel: children list empty");
    TEST_ASSERT(list_empty(&p->child_wq.head), "proc_create_kernel: child_wq empty");
    TEST_ASSERT(list_empty(&p->vma_list), "proc_create_kernel: vma_list empty");
    TEST_ASSERT(list_empty(&p->tsleep_link), "proc_create_kernel: tsleep_link self-pointing");
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
    TEST_ASSERT(list_empty(&up->vma_list), "proc_create_user: vma_list empty");
    extern void user_proc_start(void);
    TEST_ASSERT(up->context.ra == (uint64)user_proc_start,
                "proc_create_user: ra = user_proc_start");
    TEST_ASSERT(up->context.sp == up->kstack + 4096, "proc_create_user: sp = top of kstack");

    /* --- Lifecycle: exit produces zombie with correct status --- */

    struct proc *child = proc_create_kernel(exit_42_thread, "exit42");
    TEST_ASSERT(child != 0, "exit_42: created");

    /* Let it run and exit — yield until it becomes zombie */
    for (int i = 0; i < 100; i++) {
        yield();
        spin_lock(&child->lock);
        int done = (child->state == PROC_ZOMBIE);
        spin_unlock(&child->lock);
        if (done)
            break;
    }

    spin_lock(&child->lock);
    TEST_ASSERT(child->state == PROC_ZOMBIE, "exit_42: becomes zombie");
    TEST_ASSERT(child->exit_status == 42, "exit_42: exit_status == 42");
    spin_unlock(&child->lock);

    /* --- Lifecycle: proc_kill wakes sleeping process --- */

    wq_init(&kill_wq, "kill_wq");
    spin_init(&kill_lk, "kill_lk");
    kill_cond = 0;

    struct proc *victim = proc_create_kernel(sleep_forever_thread, "victim");
    TEST_ASSERT(victim != 0, "kill victim created");
    int victim_pid = victim->pid;

    /* Let it run and fall asleep */
    for (int i = 0; i < 100; i++) {
        yield();
        spin_lock(&victim->lock);
        int asleep = (victim->state == PROC_SLEEPING);
        spin_unlock(&victim->lock);
        if (asleep)
            break;
    }

    spin_lock(&victim->lock);
    TEST_ASSERT(victim->state == PROC_SLEEPING, "victim is SLEEPING");
    spin_unlock(&victim->lock);

    /* Kill it */
    int kill_ret = proc_kill(victim_pid);
    TEST_ASSERT(kill_ret == 0, "proc_kill: returns 0 on valid PID");

    /* Let it get scheduled and exit (wq_sleep returns, proc checks killed) */
    for (int i = 0; i < 100; i++) {
        yield();
        spin_lock(&victim->lock);
        int done = (victim->state == PROC_ZOMBIE);
        spin_unlock(&victim->lock);
        if (done)
            break;
    }

    spin_lock(&victim->lock);
    TEST_ASSERT(victim->state == PROC_ZOMBIE, "killed proc became zombie");
    spin_unlock(&victim->lock);

    /* --- Yield: round-robin between threads --- */

    yield_counter = 0;

    struct proc *y1 = proc_create_kernel(yield_thread, "yield1");
    struct proc *y2 = proc_create_kernel(yield_thread, "yield2");
    TEST_ASSERT(y1 != 0 && y2 != 0, "yield threads created");

    /* Let them both run to completion (each yields 5 times) */
    for (int i = 0; i < 20; i++)
        yield();

    TEST_ASSERT(yield_counter == 10, "yield: both threads ran 5 iterations each");
}
