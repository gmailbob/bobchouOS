#ifndef STRING_H
#define STRING_H

#include "types.h"

void *memset(void *dst, int val, uint64 n);
void *memcpy(void *dst, const void *src, uint64 n);
int strcmp(const char *a, const char *b);
int strlen(const char *s);

#endif /* STRING_H */
