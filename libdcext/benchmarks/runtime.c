#include <stddef.h>

void *memset(void *dst, int value, size_t n) {
    volatile unsigned char *d = dst;
    for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    volatile unsigned char *d = dst;
    const volatile unsigned char *s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    volatile unsigned char *d = dst;
    const volatile unsigned char *s = src;
    if ((size_t)d < (size_t)s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    } else {
        while (n) { --n; d[n] = s[n]; }
    }
    return dst;
}
