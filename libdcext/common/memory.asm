.text

.globl memcpy
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.type memcpy, @function
#endif
memcpy:
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    push %rdi
    push %rsi
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rdx
#endif
    mov %rdi, %rax
    mov %rdx, %rcx
    rep movsb
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    pop %rsi
    pop %rdi
#endif
    ret
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.size memcpy, . - memcpy
#endif

.globl memmove
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.type memmove, @function
#endif
memmove:
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    push %rdi
    push %rsi
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rdx
#endif
    mov %rdi, %rax
    test %rdx, %rdx
    je .Lmove_done
    cmp %rsi, %rdi
    jbe .Lmove_forward
    mov %rdi, %r10
    sub %rsi, %r10
    cmp %rdx, %r10
    jae .Lmove_forward
.Lmove_backward:
    dec %rdx
    mov (%rsi,%rdx), %r10b
    mov %r10b, (%rdi,%rdx)
    test %rdx, %rdx
    jne .Lmove_backward
    jmp .Lmove_done
.Lmove_forward:
    mov %rdx, %rcx
    rep movsb
.Lmove_done:
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    pop %rsi
    pop %rdi
#endif
    ret
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.size memmove, . - memmove
#endif

.globl memset
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.type memset, @function
#endif
memset:
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    push %rdi
    mov %rcx, %rdi
    mov %edx, %eax
    mov %r8, %rcx
#else
    mov %rdx, %rcx
    mov %esi, %eax
#endif
    mov %rdi, %r11
    rep stosb
    mov %r11, %rax
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    pop %rdi
#endif
    ret
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.size memset, . - memset
#endif

.globl memcmp
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.type memcmp, @function
#endif
memcmp:
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    push %rdi
    push %rsi
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rdx
#endif
    xor %eax, %eax
    test %rdx, %rdx
    je .Lcompare_done
.Lcompare_next:
    movzbl (%rdi), %eax
    movzbl (%rsi), %r10d
    sub %r10d, %eax
    jne .Lcompare_done
    inc %rdi
    inc %rsi
    dec %rdx
    jne .Lcompare_next
.Lcompare_done:
#if defined(_WIN32) || defined(DCC_WINDOWS_MEMORY)
    pop %rsi
    pop %rdi
#endif
    ret
#if !defined(_WIN32) && !defined(DCC_WINDOWS_MEMORY)
.size memcmp, . - memcmp
.section .note.GNU-stack,"",@progbits
#endif
