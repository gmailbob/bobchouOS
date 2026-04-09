/*
 * solutions.c — Reference solutions for Lecture 0-3 exercises.
 *
 * For each exercise: your solution, then an alternative approach.
 * Both are correct — the differences illustrate style choices and
 * common C patterns.
 *
 * This file is not built by the Makefile. It's read-only reference.
 */

// typedef unsigned char  uint8;
// typedef unsigned short uint16;
// typedef unsigned int   uint32;
// typedef unsigned long  uint64;

/* ═══════════════════════════════════════════════════════════════════
 * Exercise 1: bitfield_extract / bitfield_insert
 * ═══════════════════════════════════════════════════════════════════
 *
 * Your solution — shift-left-then-right:
 *
 *   uint64 bitfield_extract(uint64 val, int start, int width) {
 *       return (val << (64 - start - width)) >> (64 - width);
 *   }
 *
 *   uint64 bitfield_insert(uint64 val, int start, int width, uint64 field) {
 *       val &= ~((-1UL << (64 - start - width)) >> (64 - width) << start);
 *       return val | (field << (64 - width) >> (64 - width - start));
 *   }
 *
 * Alternative — shift-right-then-mask (standard pattern from Part 7):
 *
 *   uint64 bitfield_extract(uint64 val, int start, int width) {
 *       return (val >> start) & ((1UL << width) - 1);
 *   }
 *
 *   uint64 bitfield_insert(uint64 val, int start, int width, uint64 field) {
 *       uint64 mask = ((1UL << width) - 1) << start;
 *       return (val & ~mask) | ((field << start) & mask);
 *   }
 *
 * Comparison: your extract shifts left to push unwanted high bits
 * off the top of the 64-bit register, then shifts right to position
 * the field at bit 0. Correct, but requires mental effort to trace.
 * The alternative (shift right, then mask) maps directly to the
 * "extract" recipe in the lecture: (1) shift field to position 0,
 * (2) AND with a width-bit mask.
 *
 * Your insert is particularly dense — the -1UL shift chain builds
 * the mask through three operations. The alternative names the mask
 * explicitly, making clear-then-OR obvious to read. For code that
 * manipulates page table entries dozens of times, readability wins.
 */

/* ═══════════════════════════════════════════════════════════════════
 * Exercise 2: memset / memcpy
 * ═══════════════════════════════════════════════════════════════════
 *
 * Your solution — while loop with pointer increment:
 *
 *   void *my_memset(void *dst, int val, uint64 n) {
 *       uint64 i = 0;
 *       uint8 *p = (uint8 *)dst;
 *       while(i++ < n) {
 *           *(p++) = (uint8)val;
 *       }
 *       return dst;
 *   }
 *
 *   void *my_memcpy(void *dst, const void *src, uint64 n) {
 *       uint64 i = 0;
 *       uint8 *p = (uint8 *)dst;
 *       uint8 *q = (uint8 *)src;
 *       while(i++ < n) {
 *           *(p++) = *(q++);
 *       }
 *       return dst;
 *   }
 *
 * Alternative — for loop with index:
 *
 *   void *my_memset(void *dst, int val, uint64 n) {
 *       uint8 *d = (uint8 *)dst;
 *       for (uint64 i = 0; i < n; i++)
 *           d[i] = (uint8)val;
 *       return dst;
 *   }
 *
 *   void *my_memcpy(void *dst, const void *src, uint64 n) {
 *       uint8 *d = (uint8 *)dst;
 *       const uint8 *s = (const uint8 *)src;
 *       for (uint64 i = 0; i < n; i++)
 *           d[i] = s[i];
 *       return dst;
 *   }
 *
 * Comparison: both produce the same machine code. Two issues with
 * the student version:
 *
 * 1. memcpy casts src to (uint8 *), dropping the const qualifier.
 *    Should be: const uint8 *q = (const uint8 *)src;
 *    Without const, if you accidentally wrote *q = something, the
 *    compiler wouldn't catch it — the exact scenario Part 4's const
 *    section warns about.
 *
 * 2. The while(i++ < n) idiom is correct but less readable than a
 *    for loop — the increment is hidden inside the condition. The
 *    for-loop version makes the "init; test; step" structure explicit.
 */

/* ═══════════════════════════════════════════════════════════════════
 * Exercise 3: sorted_insert
 * ═══════════════════════════════════════════════════════════════════
 *
 * Your solution — special-case head, then loop:
 *
 *   void sorted_insert(struct node **head_ptr, struct node *new_node) {
 *       if(!*head_ptr || (*head_ptr)->value > new_node->value) {
 *           new_node->next = *head_ptr;
 *           *head_ptr = new_node;
 *           return;
 *       }
 *       struct node **p = head_ptr;
 *       while ((*p)->next && (*p)->next->value < new_node->value) {
 *           p = &(*p)->next;
 *       }
 *       new_node->next = (*p)->next;
 *       (*p)->next = new_node;
 *   }
 *
 * Alternative — pointer-to-pointer pattern (no special case):
 *
 *   void sorted_insert(struct node **head_ptr, struct node *new_node) {
 *       struct node **pp = head_ptr;
 *       while (*pp != 0 && (*pp)->value < new_node->value)
 *           pp = &(*pp)->next;
 *       new_node->next = *pp;
 *       *pp = new_node;
 *   }
 *
 * Comparison: your version works but special-cases head insertion
 * with an if + early return (10 lines). This is exactly the pattern
 * Part 4 said the **pp trick avoids — "no if (target == *head)
 * special case needed."
 *
 * The alternative is 4 lines with no branches. The key insight:
 * pp always points to "the pointer field that needs to be updated."
 * It doesn't track "which node am I at" — it tracks "which pointer
 * should I overwrite when I insert." That pointer could be:
 *   - head_ptr itself (insert at head / empty list)
 *   - &some_node->next (insert in middle or at end)
 * The final *pp = new_node always writes to the right place.
 *
 * Why a plain node *p can't do this: a single pointer tracks a
 * node, but can't modify what points TO it. If you need to insert
 * at the head, you'd have to modify head_ptr — but p is a copy of
 * *head_ptr, not a reference to head_ptr itself. That forces the
 * special case. With **pp, you hold the address of the pointer
 * field, so *pp = new_node rewires whoever was pointing there.
 *
 * This exercise was specifically designed to practice this pattern.
 * Worth rewriting to internalize it.
 */

/* ═══════════════════════════════════════════════════════════════════
 * Exercise 4: format_hex
 * ═══════════════════════════════════════════════════════════════════
 *
 * Your solution — lookup table, reverse iteration (LSB to MSB):
 *
 *   void format_hex(char *buf, uint64 val) {
 *       buf[0] = '0';
 *       buf[1] = 'x';
 *       buf[18] = '\0';
 *       char table[16] = {'0','1','2','3','4','5','6','7',
 *                          '8','9','A','B','C','D','E','F'};
 *       for(uint8 i = 17; i > 1; i--) {
 *           buf[i] = table[val & 0xF];
 *           val = val >> 4;
 *       }
 *   }
 *
 * Alternative — forward iteration (MSB to LSB) with if/else:
 *
 *   void format_hex(char *buf, uint64 val) {
 *       buf[0] = '0';
 *       buf[1] = 'x';
 *       for (int i = 0; i < 16; i++) {
 *           int nibble = (val >> (60 - i * 4)) & 0xF;
 *           if (nibble < 10)
 *               buf[2 + i] = '0' + nibble;
 *           else
 *               buf[2 + i] = 'A' + (nibble - 10);
 *       }
 *       buf[18] = '\0';
 *   }
 *
 * Comparison: your approach is arguably cleaner. The lookup table
 * eliminates the if/else branch, and reverse iteration (extract
 * low nibble, shift right, repeat) avoids the (60 - i * 4)
 * arithmetic. The val >>= 4 naturally walks through nibbles from
 * least to most significant, and you fill the buffer backwards to
 * compensate. Nice technique.
 *
 * The forward-iteration alternative extracts nibbles from MSB to
 * LSB using a calculated shift amount, which maps more directly to
 * "walk left-to-right through the hex digits." Easier to reason
 * about the output order, but the shift expression is noisier.
 *
 * Both produce the same output. The lookup table approach is a
 * common pattern in production code (Linux's hex formatting uses it).
 */
