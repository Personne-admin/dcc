%include "common.inc"
start:
	mov eax, 0x12345678
	add eax, 0x00010000
	shr eax, 16
	cmp ax, 0x1235
	jne fail1
	mov eax, 0x10000
	mov ecx, 0x10000
	mul ecx
	cmp edx, 1
	jne fail2
	test eax, eax
	jnz fail3
	mov ebx, 3
	lea eax, [ebx + ebx * 2 + 1]
	cmp eax, 10
	jne fail4
	mov eax, 0xffffffff
	inc eax
	jnz fail5
	mov eax, 0x80000000
	movzx ecx, ax
	jnz fail6
	sar eax, 31
	cmp eax, 0xffffffff
	jne fail7
	PUTS ok
	EXIT 0
fail1: EXIT 1
fail2: EXIT 2
fail3: EXIT 3
fail4: EXIT 4
fail5: EXIT 5
fail6: EXIT 6
fail7: EXIT 7
%include "lib.inc"
ok: db "i386 ok", 10, 0
