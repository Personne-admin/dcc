.text
.globl _start
.type  _start, @function

.weak _DC0F1.4.main4.main1SqPCqci32s
.weak _DC0F1.4.main4.main0i32s

# TODO call _DC0F2.3.std5.debug17.set_panic_handler1pv1SCqi8uv with a linux specific panic handler

_start:
    mov (%rsp), %rsi
    lea 8(%rsp), %rdi

    and $-16, %rsp

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
