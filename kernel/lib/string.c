#include "types.h"

void *
memset(void *dst, int val, uint64 n)
{
    uint8 *p = (uint8 *)dst;
    while (n--)
        *p++ = (uint8)val;
    return dst;
}

void *
memcpy(void *dst, const void *src, uint64 n)
{
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}
