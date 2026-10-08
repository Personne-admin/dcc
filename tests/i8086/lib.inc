putc:
	push dx
	push ax
	mov dx, 0x3fd
.wait:
	in al, dx
	test al, 0x20
	jz .wait
	pop ax
	mov dx, 0x3f8
	out dx, al
	pop dx
	ret

puts:
	lodsb
	test al, al
	jz .done
	call putc
	jmp puts
.done:
	ret

hex16:
	push cx
	push ax
	mov cx, 4
.digit:
	rol ax, 4
	push ax
	and al, 15
	add al, '0'
	cmp al, '9'
	jbe .emit
	add al, 'A' - '9' - 1
.emit:
	call putc
	pop ax
	loop .digit
	pop ax
	pop cx
	ret
