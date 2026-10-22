; Test-only FreeDOS launcher.
; Usage: RUNNER.COM [LOW|BIG]
; Runs A:\T.COM or A:\T.EXE with stdout/stderr redirected to A:\OUT.TXT,
; A:\ERR.TXT and writes 'S',exitcode or 'E',errorcode to A:\STAT.TXT,
; then exits QEMU through the isa-debug-exit port.

org 0x100
bits 16
start:
    mov [psp], ds
    mov ax, 0x3C00
    mov dx, out_name
    xor cx, cx
    int 0x21
    jc fail_open
    mov [out_h], ax
    mov ax, 0x3C00
    mov dx, err_name
    xor cx, cx
    int 0x21
    jc fail_open
    mov [err_h], ax
    mov ax, 0x3C00
    mov dx, stat_name
    xor cx, cx
    int 0x21
    jc fail_open
    mov [stat_h], ax

    mov ax, 0x4500
    mov bx, 1
    int 0x21
    jc fail_open
    mov [save1], ax
    mov ax, 0x4500
    mov bx, 2
    int 0x21
    jc fail_open
    mov [save2], ax

    mov ax, 0x4600
    mov bx, [out_h]
    mov cx, 1
    int 0x21
    jc fail_open
    mov ax, 0x4600
    mov bx, [err_h]
    mov cx, 2
    int 0x21
    jc fail_open

    mov ax, [psp]
    mov ds, ax
    mov sp, 0x0800
    mov bx, 0x0100
    call parse_mode
    cmp byte [mode], 'L'
    jne not_low
    mov bx, 0xFFFF
    mov ax, [psp]
    mov es, ax
    mov ah, 0x4A
    int 0x21
    sub bx, 0x10
not_low:
    cmp byte [mode], 'B'
    jne not_big
    mov bx, 0x0300
not_big:
    mov ax, [psp]
    mov es, ax
    mov ah, 0x4A
    int 0x21
    jc fail_mem

    mov ax, [psp]
    mov ds, ax
    mov es, ax
    mov ax, [0x2C]
    mov [pb_env], ax
    mov word [pb_tail], tail_buf
    mov word [pb_tail+2], cs
    mov word [pb_fcb1], 0
    mov word [pb_fcb1+2], 0
    mov word [pb_fcb2], 0
    mov word [pb_fcb2+2], 0
    mov ax, 0x4B00
    mov dx, target_com
    mov bx, pb_env
    int 0x21
    jnc exec_ok
    mov [errcode], ax
    cmp ax, 2
    jne fail_exec
    mov ax, 0x4B00
    mov dx, target_exe
    mov bx, pb_env
    int 0x21
    jnc exec_ok
    mov [errcode], ax
    jmp fail_exec

exec_ok:
    mov ah, 0x4D
    int 0x21
    mov [status], al
    mov byte [rec], 'S'
    mov [rec+1], al
    jmp finish

fail_exec:
    mov byte [rec], 'E'
    mov al, [errcode]
    mov [rec+1], al
    jmp finish
fail_mem:
    mov [errcode], ax
    mov byte [rec], 'E'
    mov al, [errcode]
    mov [rec+1], al
    jmp finish
fail_open:
    mov [errcode], ax
    mov byte [rec], 'E'
    mov al, [errcode]
    mov [rec+1], al

finish:
    mov ax, [psp]
    mov ds, ax
    mov ax, [save1]
    mov bx, ax
    mov cx, 1
    mov ax, 0x4600
    int 0x21
    mov ax, [save2]
    mov bx, ax
    mov cx, 2
    mov ax, 0x4600
    int 0x21
    mov bx, [stat_h]
    cmp bx, 0xFFFF
    je done
    mov ax, 0x4000
    mov cx, 2
    mov dx, rec
    int 0x21
    mov ax, 0x3E00
    mov bx, [stat_h]
    int 0x21
done:
    mov dx, 0x501
    mov al, 0x10
    out dx, al
    hlt

parse_mode:
    mov byte [mode], 'N'
    mov cl, [0x80]
    xor ch, ch
    cmp cx, 3
    jb ret_mode
    mov si, 0x81
    mov al, [si]
    cmp al, ' '
    jne check_l
    inc si
    dec cx
check_l:
    cmp byte [si], 'L'
    jne check_b
    cmp byte [si+1], 'O'
    jne check_b
    cmp byte [si+2], 'W'
    jne check_b
    mov byte [mode], 'L'
    ret
check_b:
    cmp byte [si], 'B'
    jne ret_mode
    cmp byte [si+1], 'I'
    jne ret_mode
    cmp byte [si+2], 'G'
    jne ret_mode
    mov byte [mode], 'B'
ret_mode:
    ret

psp:      dw 0
out_h:    dw 0
err_h:    dw 0
stat_h:   dw 0xFFFF
save1:    dw 0
save2:    dw 0
status:   db 0
errcode:  dw 0
mode:     db 'N'
rec:      db 'E', 0

target_com: db 'T.COM', 0
target_exe: db 'T.EXE', 0
out_name: db 'A:\OUT.TXT', 0
err_name: db 'A:\ERR.TXT', 0
stat_name: db 'A:\STAT.TXT', 0
tail_buf: db 4, ' ARG', 13

pb_env:   dw 0
pb_tail:  dd 0
pb_fcb1:  dd 0
pb_fcb2:  dd 0
