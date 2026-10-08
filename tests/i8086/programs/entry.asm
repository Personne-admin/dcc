%include "common.inc"
start:
	mov bx, sp
	pushf
	pop dx
	shr dx, 10
	and dx, 1
	call here
here:
	pop cx
	sub cx, here - start
	PUTS s_cs
	PUTHEX cs
	PUTS s_ds
	PUTHEX ds
	PUTS s_es
	PUTHEX es
	PUTS s_ss
	PUTHEX ss
	PUTS s_fs
	PUTHEX fs
	PUTS s_gs
	PUTHEX gs
	PUTS s_sp
	PUTHEX bx
	PUTS s_df
	PUTHEX dx
	PUTS s_ip
	PUTHEX cx
	PUTS s_end
	EXIT 0
%include "lib.inc"
s_cs: db "CS=", 0
s_ds: db " DS=", 0
s_es: db " ES=", 0
s_ss: db " SS=", 0
s_fs: db " FS=", 0
s_gs: db " GS=", 0
s_sp: db " SP=", 0
s_df: db " DF=", 0
s_ip: db " IP=", 0
s_end: db 10, 0
