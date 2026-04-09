/*
 * main.c — Test harness for Lecture 0-3 C exercises.
 *
 * Calls each exercise function with several test cases and prints
 * PASS or FAIL over UART. Uses the bare-metal UART setup from
 * Lecture 0-1.
 */

/* Type definitions — same as xv6 */
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long uint64;
typedef long int64;

/* NULL */
#define NULL ((void *)0)

/* UART address on QEMU "virt" machine */
#define UART_BASE ((volatile char *)0x10000000)

/* Declarations of exercise functions (defined in exercises.c) */
uint64 bitfield_extract(uint64 val, int start, int width);
uint64 bitfield_insert(uint64 val, int start, int width, uint64 field);
void *my_memset(void *dst, int val, uint64 n);
void *my_memcpy(void *dst, const void *src, uint64 n);

struct node {
    int value;
    struct node *next;
};
void sorted_insert(struct node **head_ptr, struct node *new_node);

void format_hex(char *buf, uint64 val);

/* ── Minimal UART output ── */

static void
uart_putc(char c) {
    *UART_BASE = c;
}

static void
uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

static void
uart_putnum(int64 n) {
    if (n < 0) {
        uart_putc('-');
        n = -n;
    }
    if (n >= 10)
        uart_putnum(n / 10);
    uart_putc('0' + (n % 10));
}

static void
uart_puthex(uint64 n) {
    uart_puts("0x");
    /* Print 64-bit value in hex */
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int nibble = (n >> i) & 0xF;
        if (nibble || started || i == 0) {
            started = 1;
            if (nibble < 10)
                uart_putc('0' + nibble);
            else
                uart_putc('A' + nibble - 10);
        }
    }
}

/* ── Test infrastructure ── */

static int tests_passed = 0;
static int tests_total = 0;

static void
check_uint64(const char *name, uint64 got, uint64 expected) {
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
        uart_puthex(got);
        uart_puts(", expected ");
        uart_puthex(expected);
        uart_puts(")");
    }
    uart_puts("\n");
}

static void
check_str(const char *name, const char *got, const char *expected) {
    tests_total++;
    /* Simple string compare */
    const char *a = got;
    const char *b = expected;
    int eq = 1;
    while (*a && *b) {
        if (*a != *b) {
            eq = 0;
            break;
        }
        a++;
        b++;
    }
    if (*a != *b)
        eq = 0;

    if (eq) {
        tests_passed++;
        uart_puts("  PASS  ");
    } else {
        uart_puts("  FAIL  ");
    }
    uart_puts(name);
    if (!eq) {
        uart_puts("  (got \"");
        uart_puts(got);
        uart_puts("\", expected \"");
        uart_puts(expected);
        uart_puts("\")");
    }
    uart_puts("\n");
}

static void
check_mem(const char *name, const void *got, const void *expected, uint64 n) {
    tests_total++;
    const uint8 *a = (const uint8 *)got;
    const uint8 *b = (const uint8 *)expected;
    int eq = 1;
    for (uint64 i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            eq = 0;
            break;
        }
    }
    if (eq) {
        tests_passed++;
        uart_puts("  PASS  ");
    } else {
        uart_puts("  FAIL  ");
    }
    uart_puts(name);
    uart_puts("\n");
}

/* ── Test cases ── */

static void
test_bitfield_extract(void) {
    uart_puts("\n--- Exercise 1a: bitfield_extract ---\n");
    check_uint64("extract(0xABCD, 8, 4)", bitfield_extract(0xABCD, 8, 4), 0xB);
    check_uint64("extract(0xABCD, 0, 8)", bitfield_extract(0xABCD, 0, 8), 0xCD);
    check_uint64("extract(0xABCD, 4, 8)", bitfield_extract(0xABCD, 4, 8), 0xBC);
    check_uint64("extract(0xDEADBEEF, 16, 16)",
                 bitfield_extract(0xDEADBEEF, 16, 16), 0xDEAD);
    check_uint64("extract(0x8000000000000000, 63, 1)",
                 bitfield_extract(0x8000000000000000UL, 63, 1), 1);
}

static void
test_bitfield_insert(void) {
    uart_puts("\n--- Exercise 1b: bitfield_insert ---\n");
    check_uint64("insert(0xABCD, 8, 4, 0xF)",
                 bitfield_insert(0xABCD, 8, 4, 0xF), 0xAFCD);
    check_uint64("insert(0xABCD, 0, 8, 0xFF)",
                 bitfield_insert(0xABCD, 0, 8, 0xFF), 0xABFF);
    check_uint64("insert(0x0000, 4, 4, 0xA)",
                 bitfield_insert(0x0000, 4, 4, 0xA), 0x00A0);
    check_uint64("insert(0xFFFF, 8, 4, 0x0)",
                 bitfield_insert(0xFFFF, 8, 4, 0x0), 0xF0FF);
    check_uint64("insert(0, 60, 4, 0xD)", bitfield_insert(0, 60, 4, 0xD),
                 0xD000000000000000UL);
}

static void
test_memset(void) {
    uart_puts("\n--- Exercise 2a: my_memset ---\n");

    uint8 buf1[8];
    uint8 expected1[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    my_memset(buf1, 0xAA, 8);
    check_mem("memset(buf, 0xAA, 8)", buf1, expected1, 8);

    uint8 buf2[4];
    uint8 expected2[4] = {0, 0, 0, 0};
    my_memset(buf2, 0, 4);
    check_mem("memset(buf, 0, 4)", buf2, expected2, 4);

    uint8 buf3[1];
    my_memset(buf3, 0x42, 1);
    check_uint64("memset(buf, 0x42, 1) → buf[0]", buf3[0], 0x42);

    /* Check return value */
    uint8 buf4[4];
    void *ret = my_memset(buf4, 0, 4);
    check_uint64("memset returns dst", (uint64)ret, (uint64)buf4);
}

static void
test_memcpy(void) {
    uart_puts("\n--- Exercise 2b: my_memcpy ---\n");

    uint8 src1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8 dst1[8];
    my_memcpy(dst1, src1, 8);
    check_mem("memcpy 8 bytes", dst1, src1, 8);

    uint8 src2[3] = {0xDE, 0xAD, 0x00};
    uint8 dst2[3];
    my_memcpy(dst2, src2, 3);
    check_mem("memcpy 3 bytes", dst2, src2, 3);

    /* Check return value */
    uint8 src3[4] = {1, 2, 3, 4};
    uint8 dst3[4];
    void *ret = my_memcpy(dst3, src3, 4);
    check_uint64("memcpy returns dst", (uint64)ret, (uint64)dst3);

    /* Copy single byte */
    uint8 src4 = 0xFF;
    uint8 dst4 = 0;
    my_memcpy(&dst4, &src4, 1);
    check_uint64("memcpy 1 byte", dst4, 0xFF);
}

/* Helper: build a list from an array of values */
static void
list_to_str(struct node *head, char *buf, int bufsize) {
    char *p = buf;
    char *end = buf + bufsize - 1;
    while (head && p < end - 4) {
        /* Simple int-to-string for small positive/negative numbers */
        int v = head->value;
        if (v < 0) {
            *p++ = '-';
            v = -v;
        }
        if (v >= 100 && p < end)
            *p++ = '0' + (v / 100) % 10;
        if (v >= 10 && p < end)
            *p++ = '0' + (v / 10) % 10;
        if (p < end)
            *p++ = '0' + v % 10;

        head = head->next;
        if (head && p < end - 1) {
            *p++ = ',';
        }
    }
    *p = '\0';
}

static void
test_sorted_insert(void) {
    uart_puts("\n--- Exercise 3: sorted_insert ---\n");
    char buf[64];

    /* Test 1: insert into empty list */
    {
        struct node *head = NULL;
        struct node n1 = {5, NULL};
        sorted_insert(&head, &n1);
        list_to_str(head, buf, 64);
        check_str("insert 5 into empty", buf, "5");
    }

    /* Test 2: insert at beginning */
    {
        struct node n2 = {3, NULL};
        struct node n1 = {5, NULL};
        n2.next = &n1;
        struct node *head = &n2;
        struct node new_node = {1, NULL};
        sorted_insert(&head, &new_node);
        list_to_str(head, buf, 64);
        check_str("insert 1 before [3,5]", buf, "1,3,5");
    }

    /* Test 3: insert in middle */
    {
        struct node n3 = {5, NULL};
        struct node n2 = {3, &n3};
        struct node n1 = {1, &n2};
        struct node *head = &n1;
        struct node new_node = {4, NULL};
        sorted_insert(&head, &new_node);
        list_to_str(head, buf, 64);
        check_str("insert 4 into [1,3,5]", buf, "1,3,4,5");
    }

    /* Test 4: insert at end */
    {
        struct node n2 = {3, NULL};
        struct node n1 = {1, &n2};
        struct node *head = &n1;
        struct node new_node = {7, NULL};
        sorted_insert(&head, &new_node);
        list_to_str(head, buf, 64);
        check_str("insert 7 after [1,3]", buf, "1,3,7");
    }

    /* Test 5: insert duplicate value */
    {
        struct node n2 = {3, NULL};
        struct node n1 = {1, &n2};
        struct node *head = &n1;
        struct node new_node = {3, NULL};
        sorted_insert(&head, &new_node);
        list_to_str(head, buf, 64);
        check_str("insert 3 into [1,3]", buf, "1,3,3");
    }
}

static void
test_format_hex(void) {
    uart_puts("\n--- Exercise 4: format_hex ---\n");
    char buf[20];

    format_hex(buf, 0);
    check_str("format_hex(0)", buf, "0x0000000000000000");

    format_hex(buf, 255);
    check_str("format_hex(255)", buf, "0x00000000000000FF");

    format_hex(buf, 0xDEADBEEF);
    check_str("format_hex(0xDEADBEEF)", buf, "0x00000000DEADBEEF");

    format_hex(buf, 0x10000000);
    check_str("format_hex(0x10000000)", buf, "0x0000000010000000");

    format_hex(buf, 0xFFFFFFFFFFFFFFFFUL);
    check_str("format_hex(0xFFFF...)", buf, "0xFFFFFFFFFFFFFFFF");
}

/* ── Entry point ── */

void
main(void) {
    uart_puts("=== Lecture 0-3: C Exercises ===\n");

    test_bitfield_extract();
    test_bitfield_insert();
    test_memset();
    test_memcpy();
    test_sorted_insert();
    test_format_hex();

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
