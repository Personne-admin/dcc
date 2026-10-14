#include <stddef.h>
#include <stdint.h>

size_t strlen(const char *p)
{
    size_t n = 0;
    while (p[n]) ++n;
    return n;
}

void *memset(void *p, int value, size_t n)
{
    unsigned char *dst = p;
    for (size_t i = 0; i < n; ++i) dst[i] = (unsigned char)value;
    return p;
}

void *memcpy(void *p, const void *q, size_t n)
{
    unsigned char *dst = p;
    const unsigned char *src = q;
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    return p;
}

void *memmove(void *p, const void *q, size_t n)
{
    unsigned char *dst = p;
    const unsigned char *src = q;
    if ((uintptr_t)dst < (uintptr_t)src) for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    else for (size_t i = n; i; --i) dst[i - 1] = src[i - 1];
    return p;
}

int memcmp(const void *p, const void *q, size_t n)
{
    const unsigned char *a = p, *b = q;
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return (int)a[i] - b[i];
    return 0;
}
