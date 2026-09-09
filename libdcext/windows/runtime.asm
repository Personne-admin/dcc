.text
.globl __chkstk
__chkstk:
    push %rcx
    push %rax
    lea 24(%rsp), %rcx
1:
    cmp $4096, %rax
    jb 2f
    sub $4096, %rcx
    test %rax, (%rcx)
    sub $4096, %rax
    jmp 1b
2:
    sub %rax, %rcx
    test %rax, (%rcx)
    pop %rax
    pop %rcx
    ret

.data
.globl _fltused
_fltused:
    .long 0

.text
.globl memcpy
memcpy:
    push %rdi
    push %rsi
    mov %rcx, %rax
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rcx
    rep movsb
    pop %rsi
    pop %rdi
    ret

.globl memset
memset:
    push %rdi
    mov %rcx, %r11
    mov %rcx, %rdi
    mov %r8, %rcx
    mov %rdx, %rax
    rep stosb
    mov %r11, %rax
    pop %rdi
    ret
