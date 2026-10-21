bits 16
org 0

start:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x7c00
    mov ds, ax
    mov word [0x21*4], interrupt21
    mov word [0x21*4+2], 0x0800
    mov ax, LOAD_SEG
    mov es, ax
    xor di, di
    mov dx, MEMORY_PARAS
.poison:
    mov cx, 16
    mov al, 0xa5
    rep stosb
    test di, di
    jnz .next
    mov ax, es
    add ax, 0x1000
    mov es, ax
.next:
    dec dx
    jnz .poison
    mov ax, PSP_SEG
    mov es, ax
    xor di, di
    mov cx, 128
    xor ax, ax
    rep stosw
    mov word [es:0], 0x20cd
    mov word [es:2], END_SEG
    mov byte [es:0x80], TAIL_SIZE
    mov si, tail
    mov di, 0x81
    push cs
    pop ds
    mov cx, TAIL_SIZE+1
    rep movsb
    mov bx, image
    mov ecx, IMAGE_SIZE
%if IS_MZ
    movzx eax, word [cs:image+8]
    shl eax, 4
    add bx, ax
    sub ecx, eax
%endif
    mov ax, bx
    shr ax, 4
    add ax, 0x0800
    mov ds, ax
    mov si, bx
    and si, 15
    mov ax, LOAD_SEG
    mov es, ax
    mov di, IMAGE_OFF
.copy:
    movsb
    test si, si
    jnz .source_ok
    mov ax, ds
    add ax, 0x1000
    mov ds, ax
.source_ok:
    test di, di
    jnz .dest_ok
    mov ax, es
    add ax, 0x1000
    mov es, ax
.dest_ok:
    dec ecx
    jnz .copy
%if IS_MZ
    mov cx, [cs:image+6]
    mov bx, [cs:image+24]
    add bx, image
.relocate:
    jcxz .relocated
    mov di, [cs:bx]
    mov ax, [cs:bx+2]
    add ax, LOAD_SEG
    mov es, ax
    add word [es:di], LOAD_SEG
    add bx, 4
    loop .relocate
.relocated:
    mov ax, [cs:image+14]
    add ax, LOAD_SEG
    mov ss, ax
    mov sp, [cs:image+16]
    mov ax, [cs:image+22]
    add ax, LOAD_SEG
    push ax
    push word [cs:image+20]
%else
    mov ax, PSP_SEG
    mov ss, ax
    mov sp, 0xfffe
    mov word [ss:0xfffe], 0
    push ax
    push word 0x100
%endif
    mov ax, PSP_SEG
    mov ds, ax
    mov es, ax
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
    xor si, si
    xor di, di
    xor bp, bp
    cld
    sti
    retf

interrupt21:
    pusha
    cld
    mov bp, sp
    push ds
    push es
    cmp ah, 0x4c
    je exit
    cmp ah, 0x51
    je .psp
    cmp ah, 0x62
    je .psp
    cmp ah, 0x40
    jne unsupported
    cmp bx, 1
    je .write
    cmp bx, 2
    jne unsupported
.write:
    mov si, dx
    mov [ss:bp+14], cx
    jcxz .done
.byte:
    mov al, bl
    call serial
    lodsb
    call serial
    loop .byte
    jmp .done
.psp:
    mov word [ss:bp+8], PSP_SEG
.done:
    and word [ss:bp+20], 0xfffe
    pop es
    pop ds
    popa
    iret
unsupported:
    mov al, 4
    call serial
    mov al, ah
    call serial
    jmp finish
exit:
    mov ah, al
    mov al, 3
    call serial
    mov al, ah
    call serial
finish:
    xor al, al
    out 0xf4, al
    cli
    hlt
    jmp finish
serial:
    push ax
    push dx
    mov ah, al
    mov dx, 0x3fd
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3f8
    mov al, ah
    out dx, al
    pop dx
    pop ax
    ret

tail: incbin TAIL_FILE
image: incbin IMAGE_FILE
