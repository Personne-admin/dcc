.text
.globl test_memcpy
test_memcpy:
    jmp memcpy
.globl test_memmove
test_memmove:
    jmp memmove
.globl test_memset
test_memset:
    jmp memset
.globl test_memcmp
test_memcmp:
    jmp memcmp
#ifndef _WIN32
.section .note.GNU-stack,"",@progbits
#endif
