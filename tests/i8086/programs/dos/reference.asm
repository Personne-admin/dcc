bits 16
org 0
%ifndef EXIT_CODE
%define EXIT_CODE 237
%endif
%ifndef STACK_SIZE
%define STACK_SIZE 256
%endif
%include "programs/dos/mz.inc"
MZ_HEADER 2
MZ_RELOC ds_word
MZ_RELOC far_word
align 16, db 0
header_end:
entry:
    mov bx, ds
    mov ah, 0x62
    int 0x21
    mov ax, es
    cmp ax, bx
    jne fail
    cmp byte [es:0x80], 4
    jne fail
    cmp dword [es:0x81], ' arg'
    jne fail
    cmp byte [es:0x85], 13
    jne fail
    db 0xb8
ds_word: dw (data_start - header_end) / 16
    mov ds, ax
    mov bx, ss
    cmp ax, bx
    jne fail
    mov ax, cs
    mov bx, ds
    cmp ax, bx
    je fail
    cmp byte [bss - data_start], 0xa5
    jne fail
    mov byte [bss - data_start], 0
    les di, [far_pointer - data_start]
    cmp byte [es:di], 'M'
    jne fail
    mov eax, 0x12345678
    cmp eax, 0x12345678
    jne fail
    mov bx, 1
    mov dx, message - data_start
    mov cx, message_end - message
    mov ah, 0x40
    int 0x21
    jc fail
    cmp ax, message_end - message
    jne fail
    mov bx, 2
    mov cx, 2
    mov ah, 0x40
    int 0x21
    mov ax, 0x4c00 + EXIT_CODE
    int 0x21
fail:
    mov ax, 0x4c79
    int 0x21
align 16, db 0
data_start:
message: db 'MZ', 0, 0x80, 13, 10
message_end:
    db 0x7f
far_pointer: dw message - data_start
far_word: dw (data_start - header_end) / 16
align 16, db 0
payload_end:
file_end:
absolute payload_end
bss: resb 16
stack_bottom: resb STACK_SIZE
stack_top:
memory_end:
