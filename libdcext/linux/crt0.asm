.text
.globl _start
.type  _start, @function

.weak _DC0F1.4.main4.main1SqPCqci32s
.weak _DC0F1.4.main4.main0i32s

_start:
    mov (%rsp), %r14
    lea 8(%rsp), %r15

    and $-16, %rsp

    call __dc_rt_init

    mov %r14, %rsi
    mov %r15, %rdi

    mov _DC0F1.4.main4.main1SqPCqci32s@GOTPCREL(%rip), %rax
    test %rax, %rax
    je 1f

    call *%rax
    jmp .exit

1:
    mov _DC0F1.4.main4.main0i32s@GOTPCREL(%rip), %rax
    test %rax, %rax
    je .missing_main
    call *%rax
    jmp .exit

.missing_main:
    mov $127, %eax

.exit:
    movslq %eax, %rdi
    mov $60, %eax
    syscall
.size _start, . - _start
