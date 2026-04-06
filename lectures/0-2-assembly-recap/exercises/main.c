/*
 * main.c — Test harness for Lecture 0-2 assembly exercises.
 *
 * Calls each assembly function with several test cases and prints
 * PASS or FAIL over UART. Uses the bare-metal UART setup from
 * Lecture 0-1.
 */

/* UART address on QEMU "virt" machine */
#define UART_BASE ((volatile char *)0x10000000)

/* Declarations of assembly functions (defined in exercises.S) */
long add_three(long a, long b, long c);
long strlen(const char *s);
long max_array(const long *arr, long n);
long sum_strlen(const char *s1, const char *s2);

/* ── Minimal UART output ── */

static void uart_putc(char c) {
    *UART_BASE = c;
}

static void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

static void uart_putnum(long n) {
    if (n < 0) {
        uart_putc('-');
        n = -n;
    }
    if (n >= 10)
        uart_putnum(n / 10);
    uart_putc('0' + (n % 10));
}

/* ── Test infrastructure ── */

static int tests_passed = 0;
static int tests_total  = 0;

static void check(const char *name, long got, long expected) {
    tests_total++;
    if (got == expected) {
        tests_passed++;
        uart_puts("  PASS  ");
    } else {
        uart_puts("  FAIL  ");
    }
    uart_puts(name);
    if (got != expected) {
        uart_puts("  (got ");
        uart_putnum(got);
        uart_puts(", expected ");
        uart_putnum(expected);
        uart_puts(")");
    }
    uart_puts("\n");
}

/* ── Test cases ── */

static void test_add_three(void) {
    uart_puts("\n--- Exercise 1: add_three ---\n");
    check("add_three(1, 2, 3)",        add_three(1, 2, 3),        6);
    check("add_three(0, 0, 0)",        add_three(0, 0, 0),        0);
    check("add_three(-1, 1, 0)",       add_three(-1, 1, 0),       0);
    check("add_three(100, 200, 300)",  add_three(100, 200, 300),  600);
    check("add_three(-10, -20, -30)",  add_three(-10, -20, -30), -60);
}

static void test_strlen(void) {
    uart_puts("\n--- Exercise 2: strlen ---\n");
    check("strlen(\"\")",             strlen(""),             0);
    check("strlen(\"H\")",           strlen("H"),            1);
    check("strlen(\"Hi\")",          strlen("Hi"),           2);
    check("strlen(\"Hello\")",       strlen("Hello"),        5);
    check("strlen(\"bobchouOS\")",   strlen("bobchouOS"),    9);
}

static void test_max_array(void) {
    uart_puts("\n--- Exercise 3: max_array ---\n");

    long a1[] = {42};
    check("max_array([42], 1)",         max_array(a1, 1),   42);

    long a2[] = {3, 7, 2};
    check("max_array([3,7,2], 3)",      max_array(a2, 3),   7);

    long a3[] = {10, 5, 8, 3, 1};
    check("max_array([10,5,8,3,1], 5)", max_array(a3, 5),   10);

    long a4[] = {1, 2, 3, 4, 5};
    check("max_array([1,2,3,4,5], 5)",  max_array(a4, 5),   5);

    long a5[] = {-3, -1, -7, -2};
    check("max_array([-3,-1,-7,-2], 4)", max_array(a5, 4),  -1);
}

static void test_sum_strlen(void) {
    uart_puts("\n--- Exercise 4: sum_strlen ---\n");
    check("sum_strlen(\"\", \"\")",           sum_strlen("", ""),           0);
    check("sum_strlen(\"Hi\", \"\")",         sum_strlen("Hi", ""),         2);
    check("sum_strlen(\"\", \"World\")",      sum_strlen("", "World"),      5);
    check("sum_strlen(\"Hi\", \"World\")",    sum_strlen("Hi", "World"),    7);
    check("sum_strlen(\"abc\", \"defgh\")",   sum_strlen("abc", "defgh"),   8);
}

/* ── Entry point ── */

void main(void) {
    uart_puts("=== Lecture 0-2: Assembly Exercises ===\n");

    test_add_three();
    test_strlen();
    test_max_array();
    test_sum_strlen();

    uart_puts("\n--- Results: ");
    uart_putnum(tests_passed);
    uart_puts("/");
    uart_putnum(tests_total);
    if (tests_passed == tests_total)
        uart_puts(" ALL PASSED!");
    else
        uart_puts(" (some failed)");
    uart_puts(" ---\n");
}
