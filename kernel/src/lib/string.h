#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>

/* the usual suspects. no libc here so we roll our own.
 * note: gcc can emit calls to memcpy/memset behind your back even in
 * freestanding mode (struct copies etc), so these have to exist */

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
