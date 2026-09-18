bits 16
org 100h

start:
    mov si, data_word
    mov word [si], 0x1234
    mov ax, [si]
    mov dx, 0x0080
    out dx, al
    in al, dx
    mov ax, 0x4c00
    int 0x21

data_word:
    dw 0
