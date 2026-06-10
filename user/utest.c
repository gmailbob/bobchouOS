/*
 * utest.c — User-space integration tests for bobchouOS.
 *
 * Each test_* function exercises one feature and prints PASS/FAIL.
 * init.c execs "utest" to run all tests.
 *
 * Round 6-3: write, sleep, argv
 * Round 6-4: sbrk (heap), COW fork, demand paging, elastic stack
 */

#include "user.h"

/* --- Helpers --- */

static void
print(const char *s) {
    int len = 0;
    while (s[len])
        len++;
    write(1, s, len);
}

static void
pass(const char *name) {
    print("[utest] ");
    print(name);
    print(": PASS\n");
}

static void
fail(const char *name) {
    print("[utest] ");
    print(name);
    print(": FAIL\n");
    exit(1);
}

/* --- Round 6-3 tests --- */

static void
test_hello(void) {
    print("hello, bobchouOS!\n");
    pass("hello");
}

static void
test_sleep(void) {
    sleep(100);
    pass("sleep");
}

/* --- Round 6-4 tests --- */

static void
test_sbrk_grow_shrink(void) {
    char *p = sbrk(4096);
    if ((long)p == -1)
        fail("sbrk_grow_shrink: grow");

    /* Write to lazily-allocated page */
    p[0] = 'H';
    p[1] = 'i';

    if (p[0] != 'H' || p[1] != 'i')
        fail("sbrk_grow_shrink: read back");

    /* Shrink back */
    char *q = sbrk(-4096);
    if ((long)q == -1)
        fail("sbrk_grow_shrink: shrink");

    pass("sbrk_grow_shrink");
}

static void
test_lazy_alloc(void) {
    /* Allocate 16 pages but only touch 2 */
    char *base = sbrk(16 * 4096);
    if ((long)base == -1)
        fail("lazy_alloc: sbrk");

    base[3 * 4096] = 'A';
    base[7 * 4096] = 'B';

    if (base[3 * 4096] != 'A' || base[7 * 4096] != 'B')
        fail("lazy_alloc: read back");

    /* Clean up */
    sbrk(-16 * 4096);
    pass("lazy_alloc");
}

static void
test_elastic_stack(void) {
    /* Touch memory beyond the first stack page to trigger demand paging.
     * A single large local forces sp well below the initial mapped page. */
    volatile char big[8192];
    big[0] = 'S';
    big[8191] = 'E';
    if (big[0] != 'S' || big[8191] != 'E')
        fail("elastic_stack");
    pass("elastic_stack");
}

static void
test_cow_fork(void) {
    int *p = (int *)sbrk(4096);
    if ((long)p == -1)
        fail("cow_fork: sbrk");
    *p = 42;

    int pid = fork();
    if (pid < 0)
        fail("cow_fork: fork");

    if (pid == 0) {
        /* Child: should see original value */
        if (*p != 42)
            fail("cow_fork: child read");

        /* Let parent write (trigger COW) */
        sleep(100);

        /* After parent COW, child should still see 42 */
        if (*p != 42)
            fail("cow_fork: child post-cow");

        print("[utest] cow_fork child: PASS\n");
        exit(0);
    }

    /* Parent: trigger COW by writing */
    sleep(50);
    *p = 99;

    int status;
    wait(&status);

    if (*p != 99)
        fail("cow_fork: parent read");

    sbrk(-4096);
    pass("cow_fork");
}

static void
test_fork_exec(void) {
    /* Verify COW pages are released properly on exec.
     * Fork, child execs back into utest with "exec_child" arg,
     * parent writes after child execs (should be refcount=1 fast path). */
    int *p = (int *)sbrk(4096);
    if ((long)p == -1)
        fail("fork_exec: sbrk");
    *p = 77;

    int pid = fork();
    if (pid < 0)
        fail("fork_exec: fork");

    if (pid == 0) {
        /* Child execs — releases COW references */
        char *argv[] = {"utest", "exec_child", 0};
        exec("utest", argv);
        fail("fork_exec: exec failed");
    }

    /* Wait for child to exec and exit */
    int status;
    wait(&status);

    /* Parent writes — should be sole owner (refcount==1, no copy needed) */
    *p = 88;
    if (*p != 88)
        fail("fork_exec: parent write");

    sbrk(-4096);
    pass("fork_exec");
}

int
main(int argc, char *argv[]) {
    /* If called as "utest exec_child", just exit — used by test_fork_exec */
    if (argc > 1 && argv[1][0] == 'e') {
        exit(0);
    }

    test_hello();
    test_sleep();
    test_sbrk_grow_shrink();
    test_lazy_alloc();
    test_elastic_stack();
    test_cow_fork();
    test_fork_exec();

    print("[utest] all tests passed\n");
    exit(0);
}
