bits 16
org 0x100

mov ax, 0x3000        ; Earlier INT 21h call must not match AH=4Ch.
int 0x21
mov ax, 0x4c07        ; DOS terminate, exit code 7.
int 0x21
