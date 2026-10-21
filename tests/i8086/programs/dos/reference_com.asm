bits 16
org 0x100
%ifndef EXIT_CODE
%define EXIT_CODE 255
%endif
    mov ax, cs
    mov bx, ds
    cmp ax, bx
    jne fail
    mov bx, ss
    cmp ax, bx
    jne fail
    cmp sp, 0xfffe
    jne fail
    mov ah, 0x51
    int 0x21
    mov ax, es
    cmp bx, ax
    jne fail
    cmp byte [0x80], 4
    jne fail
    cmp dword [0x81], ' arg'
    jne fail
    mov eax, 0x12345678
    cmp eax, 0x12345678
    jne fail
    mov bx, 1
    mov dx, message
    mov cx, 6
    mov ah, 0x40
    int 0x21
    jc fail
    cmp ax, 6
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
message: db 'CO', 0, 0x80, 13, 10
