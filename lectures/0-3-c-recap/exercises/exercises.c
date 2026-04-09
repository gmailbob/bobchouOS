/*
 * exercises.c — C exercises for Lecture 0-3
 *
 * Implement the four functions below. Each one is called from main.c
 * which prints PASS or FAIL for each test case.
 */

/* Type definitions — same as xv6 */
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long uint64;

/* ─────────────────────────────────────────────────────────────────────
 * Exercise 1: bitfield_extract / bitfield_insert
 *
 * Extract or insert a contiguous bit field within a 64-bit value.
 *
 * bitfield_extract(val, start, width):
 *   Return the 'width' bits starting at bit position 'start'.
 *   Example: bitfield_extract(0xABCD, 8, 4) → 0xB
 *             (bits [11:8] of 0xABCD = 0xB)
 *
 * bitfield_insert(val, start, width, field):
 *   Replace the 'width' bits starting at bit 'start' with 'field',
 *   leaving all other bits unchanged. Return the new value.
 *   Example: bitfield_insert(0xABCD, 8, 4, 0xF) → 0xAFCD
 *             (replace bits [11:8] with 0xF)
 *
 * Hint: use >> to shift, (1UL << width) - 1 to make a mask.
 *       Review Part 7 of the lecture (bitwise operations).
 * ───────────────────────────────────────────────────────────────────── */

uint64
bitfield_extract(uint64 val, int start, int width) {
    /* TODO: implement */
    return 0;
}

uint64
bitfield_insert(uint64 val, int start, int width, uint64 field) {
    /* TODO: implement */
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * Exercise 2: memset / memcpy
 *
 * Implement these two standard library functions from scratch.
 * We can't use <string.h> in bare-metal code, so we write our own.
 *
 * my_memset(dst, val, n):
 *   Fill 'n' bytes starting at 'dst' with the byte value 'val'.
 *   Return 'dst'.
 *
 * my_memcpy(dst, src, n):
 *   Copy 'n' bytes from 'src' to 'dst'. Return 'dst'.
 *   You may assume dst and src don't overlap.
 *
 * Hint: cast void * to uint8 * (or char *) to do byte-by-byte access.
 *       Review Part 4 (pointer casting, void *) and Part 5 (arrays).
 * ───────────────────────────────────────────────────────────────────── */

void *
my_memset(void *dst, int val, uint64 n) {
    /* TODO: implement */
    return dst;
}

void *
my_memcpy(void *dst, const void *src, uint64 n) {
    /* TODO: implement */
    return dst;
}

/* ─────────────────────────────────────────────────────────────────────
 * Exercise 3: sorted_insert
 *
 * Insert a node into a singly-linked list that is sorted in ascending
 * order by the 'value' field, maintaining the sorted order.
 *
 * The list is defined as:
 *   struct node { int value; struct node *next; };
 *
 * sorted_insert(head_ptr, new_node):
 *   Insert 'new_node' into the list so that the list remains sorted.
 *   'head_ptr' is a pointer TO the head pointer (struct node **),
 *   so you can modify the head if the new node goes first.
 *
 * Example: list = [1] → [3] → [5] → NULL
 *          insert node with value 4
 *          result = [1] → [3] → [4] → [5] → NULL
 *
 * Hint: use the pointer-to-pointer (**pp) pattern from Part 4.
 *       Walk pp until *pp is NULL or (*pp)->value >= new_node->value,
 *       then splice in new_node.
 * ───────────────────────────────────────────────────────────────────── */

struct node {
    int value;
    struct node *next;
};

void
sorted_insert(struct node **head_ptr, struct node *new_node) {
    /* TODO: implement */
}

/* ─────────────────────────────────────────────────────────────────────
 * Exercise 4: format_hex
 *
 * Convert a uint64 value to a hexadecimal string representation.
 *
 * format_hex(buf, val):
 *   Write the hex representation of 'val' into 'buf' as a
 *   null-terminated string with "0x" prefix and exactly 16 hex digits
 *   (zero-padded). Example: format_hex(buf, 255) → "0x00000000000000FF"
 *
 *   'buf' is guaranteed to be at least 19 bytes (2 for "0x" + 16 digits
 *   + 1 for '\0').
 *
 * Hint: extract each nibble (4 bits) using shifts and masks.
 *       To convert nibble 0-9 to ASCII: nibble + '0'
 *       To convert nibble 10-15 to ASCII: nibble - 10 + 'A'
 *       Work from the most significant nibble (bits 63-60) down to
 *       the least significant (bits 3-0).
 *       Review Part 7 (bit extraction) and the hex table.
 * ───────────────────────────────────────────────────────────────────── */

void
format_hex(char *buf, uint64 val) {
    /* TODO: implement */
    buf[0] = '\0';
}
