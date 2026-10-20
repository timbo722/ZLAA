/* Minimal memset/memcpy for the ARM64 side of the tests (there is no C library there).
 * The compiler may emit calls to these for struct copies and array initialisation.
 * `volatile` stops the compiler from "optimising" these loops back into memset calls. */
#include <stddef.h>

void *memset(void *dst, int v, size_t n) {
    volatile unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    volatile unsigned char *d = dst;
    const volatile unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    volatile unsigned char *d = dst;
    const volatile unsigned char *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
