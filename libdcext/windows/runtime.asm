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
