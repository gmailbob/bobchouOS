#include "types.h"

void *
memset(void *dst, int val, uint64 n) {
    uint8 *p = (uint8 *)dst;
    while (n--)
        *p++ = (uint8)val;
    return dst;
}

void *
memcpy(void *dst, const void *src, uint64 n) {
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

int
strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8)*a - (uint8)*b;
}

int
strlen(const char *s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}
