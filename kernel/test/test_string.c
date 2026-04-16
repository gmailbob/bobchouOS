/*
 * test_string.c -- Tests for string/memory functions.
 */

#include "test/test.h"
#include "types.h"
#include "string.h"

void
test_string(void) {
    kprintf("[string]\n");

    /* memset: fill buffer with a value */
    uint8 buf[16];
    memset(buf, 0xAB, sizeof(buf));
    TEST_ASSERT(buf[0] == 0xAB, "memset first byte");
    TEST_ASSERT(buf[15] == 0xAB, "memset last byte");

    /* memset: returns dst */
    TEST_ASSERT(memset(buf, 0, 4) == buf, "memset returns dst");

    /* memset: zero fill */
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT(buf[0] == 0 && buf[15] == 0, "memset zero fill");

    /* memcpy: basic copy */
    uint8 src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8 dst[8];
    memset(dst, 0, sizeof(dst));
    memcpy(dst, src, sizeof(src));
    TEST_ASSERT(dst[0] == 1 && dst[7] == 8, "memcpy basic");

    /* memcpy: returns dst */
    TEST_ASSERT(memcpy(dst, src, 4) == dst, "memcpy returns dst");

    /* memcpy: partial copy */
    memset(dst, 0, sizeof(dst));
    memcpy(dst, src, 3);
    TEST_ASSERT(dst[2] == 3 && dst[3] == 0, "memcpy partial");

    /* memset: zero length is a no-op */
    memset(buf, 0xAA, sizeof(buf));
    memset(buf, 0xFF, 0);
    TEST_ASSERT(buf[0] == 0xAA, "memset zero length no-op");

    /* memcpy: zero length is a no-op */
    memset(dst, 0, sizeof(dst));
    memcpy(dst, src, 0);
    TEST_ASSERT(dst[0] == 0, "memcpy zero length no-op");

    /* memset: single byte */
    memset(buf, 0, sizeof(buf));
    memset(buf, 0xFF, 1);
    TEST_ASSERT(buf[0] == 0xFF && buf[1] == 0, "memset single byte");

    /* memcpy: single byte */
    memset(dst, 0, sizeof(dst));
    memcpy(dst, src, 1);
    TEST_ASSERT(dst[0] == 1 && dst[1] == 0, "memcpy single byte");
}
