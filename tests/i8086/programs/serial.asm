%include "common.inc"
start:
	mov si, message
	mov cx, message_end - message
.next:
	lodsb
	call putc
	loop .next
	EXIT 0
%include "lib.inc"
message: db "serial ok", 0, 0xff, 0x80, 13, 10
message_end:
