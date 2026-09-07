.text
.globl _start
_start:
    sub $40, %rsp
    call __dc_rt_init
    call _DC0F1.4.main4.main0i32s
    mov %eax, %ecx
    call *__imp_ExitProcess(%rip)
    int3
