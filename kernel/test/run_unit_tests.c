/*
 * run_unit_tests.c — Boot-time unit test runner.
 *
 * These tests exercise pure logic (data structures, allocators, VM ops)
 * and do not require the scheduler. Called from kmain before scheduler().
 */

#include "test/test.h"

int test_pass = 0;
int test_fail = 0;

void test_kprintf(void);
void test_string(void);
void test_trap(void);
void test_kalloc(void);
void test_vm(void);
void test_kmalloc(void);
void test_list(void);
void test_hashtable(void);
void test_spinlock(void);
void test_vma(void);

void
run_unit_tests(void) {
    kprintf("\n=== unit tests ===\n\n");

    test_kprintf();
    test_string();
    test_trap();
    test_kalloc();
    test_vm();
    test_kmalloc();
    test_list();
    test_hashtable();
    test_spinlock();
    test_vma();

    kprintf("\n=== unit: %d passed, %d failed ===\n\n", test_pass, test_fail);
}
