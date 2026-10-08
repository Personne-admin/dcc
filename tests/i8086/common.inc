bits 16
%ifndef ORG
%define ORG 0
%endif
org ORG

%macro EXIT 1
	mov al, %1
	mov dx, 0xf4
	out dx, al
	jmp $
%endmacro

%macro PUTS 1
	mov si, %1
	call puts
%endmacro

%macro PUTHEX 1
	mov ax, %1
	call hex16
%endmacro
