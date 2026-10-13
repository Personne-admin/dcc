#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct Single { unsigned a:3; unsigned b:5; };
struct Several { unsigned a:16; unsigned b:16; unsigned c:16; };
struct NoFit { unsigned char a:6; unsigned char b:4; };
struct ZeroSep { unsigned a:3; unsigned :0; unsigned b:5; };
struct Mixed { unsigned a:30; unsigned char b:4; };
struct Signed { int a:3; int b:5; };
struct __attribute__((packed)) Packed { unsigned char p; unsigned b:16; unsigned c:16; };
struct Wide { unsigned long long a:40; unsigned long long b:24; };
struct Bool { _Bool p:1; _Bool q:1; unsigned v; };

static void emit(const void* p, size_t sz, size_t al) {
    uint64_t h[2] = {(uint64_t)sz, (uint64_t)al};
    fwrite(h, 1, 16, stdout);
    fwrite(p, 1, sz, stdout);
}

int main() {
    struct Single s1 = {7, 31};
    struct Several s2 = {0xABCD, 0x1234, 0x5678};
    struct NoFit s3 = {0x2A, 0x5};
    struct ZeroSep s4 = {5, 17};
    struct Mixed s5 = {0x2AAAAAAA, 0x5};
    struct Signed s6 = {-3, 7};
    struct Packed s7 = {0x12, 0xABCD, 0x3456};
    struct Wide s8 = {0x123456789AULL, 0xBCDEF0};
    struct Bool s9 = {true, false, 0x12345678};
    emit(&s1, sizeof s1, _Alignof(struct Single));
    emit(&s2, sizeof s2, _Alignof(struct Several));
    emit(&s3, sizeof s3, _Alignof(struct NoFit));
    emit(&s4, sizeof s4, _Alignof(struct ZeroSep));
    emit(&s5, sizeof s5, _Alignof(struct Mixed));
    emit(&s6, sizeof s6, _Alignof(struct Signed));
    emit(&s7, sizeof s7, _Alignof(struct Packed));
    emit(&s8, sizeof s8, _Alignof(struct Wide));
    emit(&s9, sizeof s9, _Alignof(struct Bool));
    return 0;
}
