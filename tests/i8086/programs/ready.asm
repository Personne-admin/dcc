%include "common.inc"
start:
	PUTS message
	jmp $
%include "lib.inc"
message: db "READY", 10, 0
