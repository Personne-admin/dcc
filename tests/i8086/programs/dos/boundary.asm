bits 16
org 0
%include "programs/dos/mz.inc"
MZ_HEADER 2
MZ_RELOC code_word
MZ_RELOC data_word
align 16, db 0
header_end:
    times 15 db 0
code_word: dw 0
    times 31 - ($ - header_end) db 0
data_word: dw (data_start - header_end) / 16
    times 40 - ($ - header_end) db 0
entry:
    push cs
    pop ax
    cmp [cs:code_word - header_end], ax
    jne fail
    add ax, (data_start - header_end) / 16
    cmp [cs:data_word - header_end], ax
    jne fail
    mov ax, 0x4c07
    int 0x21
fail:
    mov ax, 0x4c79
    int 0x21
    times 96 - ($ - header_end) db 0
data_start:
    times 32 db 0
payload_end:
file_end:
absolute payload_end
    resb 16
    resb 64
stack_top:
memory_end:
