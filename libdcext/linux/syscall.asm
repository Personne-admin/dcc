.text

.globl __syscall0
.type __syscall0, @function
__syscall0:
    mov %rdi, %rax
    syscall
    ret
.size __syscall0, . - __syscall0

.globl __syscall1
.type __syscall1, @function
__syscall1:
    mov %rdi, %rax
    mov %rsi, %rdi
    syscall
    ret
.size __syscall1, . - __syscall1

.globl __syscall2
.type __syscall2, @function
__syscall2:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    syscall
    ret
.size __syscall2, . - __syscall2

.globl __syscall3
.type __syscall3, @function
__syscall3:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    syscall
    ret
.size __syscall3, . - __syscall3

.globl __syscall4
.type __syscall4, @function
__syscall4:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    syscall
    ret
.size __syscall4, . - __syscall4

.globl __syscall5
.type __syscall5, @function
__syscall5:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    syscall
    ret
.size __syscall5, . - __syscall5

.globl __syscall6
.type __syscall6, @function
__syscall6:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    mov 8(%rsp), %r9
    syscall
    ret
.size __syscall6, . - __syscall6

.globl __thread_entry
.type __thread_entry, @function
__thread_entry:
    mov %rdx, %r12
    mov $56, %eax

    xor %r10d, %r10d
    xor %r8d, %r8d
    syscall
    test %rax, %rax
    jnz .thread_parent

    cld
    mov %r12, %rdi
    call thread_child_main

    mov $60, %eax
    xor %edi, %edi
    syscall
    ud2

.thread_parent:
    ret
.size __thread_entry, . - __thread_entry
