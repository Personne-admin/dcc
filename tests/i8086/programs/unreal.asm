%include "common.inc"
start:
	xor ax, ax
	mov es, ax
	mov word [es:13 * 4], fault
	mov [es:13 * 4 + 2], cs
	mov ebx, 0x00100000
	mov dword [ebx], 0xcafebabe
	mov ebx, 0x00300000
	mov dword [ebx], 0x12345678
	mov ebx, 0x02000000
	mov dword [ebx], 0xdeadbeef
	mov ebx, 0x00100000
	cmp dword [ebx], 0xcafebabe
	jne mismatch
	mov ebx, 0x00300000
	cmp dword [ebx], 0x12345678
	jne mismatch
	mov ebx, 0x02000000
	cmp dword [ebx], 0xdeadbeef
	jne mismatch
	PUTS ok
	EXIT 0
mismatch:
	PUTS bad
	EXIT 2
fault:
	PUTS faulted
	EXIT 77
%include "lib.inc"
ok: db "unreal ok", 10, 0
bad: db "unreal mismatch", 10, 0
faulted: db "unreal fault", 10, 0
