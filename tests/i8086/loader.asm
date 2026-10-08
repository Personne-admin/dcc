bits 16
org 0x7c00

EXIT_PORT	equ 0xf4
ERR_DISK	equ 124
ERR_A20		equ 125

NORM_SEG	equ LOAD_SEG + (LOAD_OFF >> 4)
NORM_OFF	equ LOAD_OFF & 15
SEL_FLAT4G	equ 0x8
SEL_LIMIT64	equ 0x10
%if UNREAL
DATA_SEL	equ SEL_FLAT4G
%else
DATA_SEL	equ SEL_LIMIT64
%endif

start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7c00
	cld
	mov [drive], dl

.read:
	mov ax, [remaining]
	cmp ax, 64
	jbe .count
	mov ax, 64
.count:
	mov [dap_count], ax
	mov si, dap
	mov dl, [drive]
	mov ah, 0x42
	int 0x13
	jc fail_disk
	movzx eax, word [dap_count]
	sub [remaining], ax
	add [dap_lba], eax
	shl ax, 5
	add [dap_seg], ax
	cmp word [remaining], 0
	jne .read

	call a20_enabled
	jnz .a20_ok
	mov ax, 0x2401
	int 0x15
	call a20_enabled
	jnz .a20_ok
	in al, 0x92
	or al, 2
	and al, 0xfe
	out 0x92, al
	mov cx, 0xffff
.a20_wait:
	call a20_enabled
	jnz .a20_ok
	loop .a20_wait
	jmp fail_a20
.a20_ok:

	cli
	xor ax, ax
	mov ds, ax
	lgdt [gdtr]
	mov eax, cr0
	or al, 1
	mov cr0, eax
	jmp short .protected
.protected:
	mov bx, DATA_SEL
	mov ds, bx
	mov es, bx
	mov fs, bx
	mov gs, bx
	mov bx, SEL_LIMIT64
	mov ss, bx
	and al, 0xfe
	mov cr0, eax
	jmp short .real
.real:

	mov ax, LOAD_SEG
	mov ds, ax
	mov es, ax
	mov ss, ax
	xor ax, ax
	mov fs, ax
	mov gs, ax
	mov esp, 0xfffe
	xor eax, eax
	xor ebx, ebx
	xor ecx, ecx
	xor edx, edx
	xor esi, esi
	xor edi, edi
	xor ebp, ebp
	cld
	jmp LOAD_SEG:LOAD_OFF

a20_enabled:
	push ds
	push es
	push si
	push di
	xor ax, ax
	mov ds, ax
	not ax
	mov es, ax
	mov si, 0x500
	mov di, 0x510
	mov al, [ds:si]
	mov ah, [es:di]
	push ax
	mov byte [ds:si], 0x00
	mov byte [es:di], 0xff
	cmp byte [ds:si], 0xff
	pop ax
	mov [es:di], ah
	mov [ds:si], al
	pop di
	pop si
	pop es
	pop ds
	ret

fail_disk:
	mov al, ERR_DISK
	jmp fail
fail_a20:
	mov al, ERR_A20
fail:
	mov dx, EXIT_PORT
	out dx, al
	cli
.halt:
	hlt
	jmp .halt

drive: db 0
remaining: dw SECTORS

align 4
dap:
	db 0x10, 0
dap_count: dw 0
dap_off: dw NORM_OFF
dap_seg: dw NORM_SEG
dap_lba: dq 1

align 4
gdtr:
	dw gdt_end - gdt - 1
	dd gdt
align 8
gdt:
	dq 0
	dq 0x008f92000000ffff
	dq 0x000092000000ffff
gdt_end:

times 510 - ($ - $$) db 0
dw 0xaa55
