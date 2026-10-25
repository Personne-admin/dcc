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

.text
.globl test_memory_abi
test_memory_abi:
    push %rbx
    push %rbp
    push %rdi
    push %rsi
    push %r12
    push %r13
    push %r14
    push %r15
    sub $168, %rsp
    mov $0x1111, %rbx
    mov $0x2222, %rbp
    mov $0x5555, %r12
    mov $0x6666, %r13
    mov $0x7777, %r14
    mov $0x8888, %r15
#ifdef _WIN32
    mov $0x3333, %rdi
    mov $0x4444, %rsi
#endif
    movabs $0x0807060504030201, %rax
    mov %rax, 96(%rsp)
    mov %rax, 104(%rsp)
.macro memory_args
#ifdef _WIN32
    lea 32(%rsp), %rcx
    lea 96(%rsp), %rdx
    mov $16, %r8
#else
    lea 32(%rsp), %rdi
    lea 96(%rsp), %rsi
    mov $16, %rdx
#endif
.endm
.macro check_saved
    cmp $0x1111, %rbx
    jne .Labi_failed
    cmp $0x2222, %rbp
    jne .Labi_failed
    cmp $0x5555, %r12
    jne .Labi_failed
    cmp $0x6666, %r13
    jne .Labi_failed
    cmp $0x7777, %r14
    jne .Labi_failed
    cmp $0x8888, %r15
    jne .Labi_failed
#ifdef _WIN32
    cmp $0x3333, %rdi
    jne .Labi_failed
    cmp $0x4444, %rsi
    jne .Labi_failed
#endif
    pushfq
    pop %r10
    test $0x400, %r10
    jne .Labi_failed
.endm
    memory_args
    call memcpy
    check_saved
    memory_args
    call memmove
    check_saved
    memory_args
#ifdef _WIN32
    mov $0x180, %edx
#else
    mov $0x180, %esi
#endif
    call memset
    check_saved
    memory_args
#ifdef _WIN32
    mov %rcx, %rdx
#else
    mov %rdi, %rsi
#endif
    call memcmp
    test %eax, %eax
    jne .Labi_failed
    check_saved
    xor %eax, %eax
    jmp .Labi_return
.Labi_failed:
    mov $1, %eax
.Labi_return:
    cld
    add $168, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rsi
    pop %rdi
    pop %rbp
    pop %rbx
    ret
#ifndef _WIN32
.section .note.GNU-stack,"",@progbits
#endif
