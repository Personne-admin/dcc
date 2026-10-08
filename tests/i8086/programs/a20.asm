%include "common.inc"
start:
	xor ax, ax
	mov es, ax
	mov ax, 0xffff
	mov fs, ax
	mov byte [es:0x600], 0x00
	mov byte [fs:0x610], 0xff
	cmp byte [es:0x600], 0xff
	je .wrapped
	PUTS enabled
	EXIT 0
.wrapped:
	PUTS wrapped
	EXIT 1
%include "lib.inc"
enabled: db "A20 on", 10, 0
wrapped: db "A20 off", 10, 0
