bits 16

section .start progbits alloc exec nowrite align=1

global _start
global dcc_putc
global dcc_exit
extern dcc_main
extern __bss_start
extern __bss_end

_start:
	cld
	mov edi, __bss_start
	mov ecx, __bss_end
	sub ecx, edi
	xor eax, eax
	a32 rep stosb
	call dcc_main
	jmp exit_with_al

dcc_putc:
	push bp
	mov bp, sp
	mov dx, 0x3fd
.wait:
	in al, dx
	test al, 0x20
	jz .wait
	mov al, [bp + 4]
	mov dx, 0x3f8
	out dx, al
	pop bp
	ret

dcc_exit:
	mov bp, sp
	mov al, [bp + 2]
exit_with_al:
	mov dx, 0xf4
	out dx, al
.hang:
	cli
	hlt
	jmp .hang
