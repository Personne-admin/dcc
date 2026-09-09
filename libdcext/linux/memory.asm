.text

.globl memcpy
.type memcpy, @function
memcpy:
    mov %rdi, %rax
    mov %rdx, %rcx
    rep movsb
    ret
.size memcpy, . - memcpy

.globl memset
.type memset, @function
memset:
    mov %rdi, %r11
    mov %rdx, %rcx
    mov %rsi, %rax
    rep stosb
    mov %r11, %rax
    ret
.size memset, . - memset
