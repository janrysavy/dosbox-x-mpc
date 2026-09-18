bits 16
org 100h

    mov cx, 5
    xor ax, ax
again:
    inc ax
target:
    nop
    loop again
    int 0x20
