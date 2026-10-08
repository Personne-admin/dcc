%include "common.inc"
start:
	mov si, message
	call puts
	jmp $
%include "lib.inc"
message: db "hanging", 10, 0
