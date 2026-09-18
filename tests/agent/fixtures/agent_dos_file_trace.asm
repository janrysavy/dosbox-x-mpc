.intel_syntax noprefix
.code16
.text
.global _start

# Known-source DOS file-service trace fixture.  It deliberately covers a
# successful create/write/seek/close, a failed open, a short read followed by
# EOF, and reuse of the first closed DOS handle for a second file.
_start:
    mov dx, offset first_name
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    jc failed
    mov bx, ax

    mov dx, offset first_payload
    mov cx, 5
    mov ah, 0x40
    int 0x21
    jc failed

    mov ax, 0x4200
    xor cx, cx
    mov dx, 2
    int 0x21
    jc failed

    mov ah, 0x3e
    int 0x21
    jc failed

    mov dx, offset missing_name
    xor ax, ax
    mov ah, 0x3d
    int 0x21
    jnc failed

    mov dx, offset first_name
    xor ax, ax
    mov ah, 0x3d
    int 0x21
    jc failed
    mov bx, ax

    mov dx, offset read_buffer
    mov cx, 4
    mov ah, 0x3f
    int 0x21
    jc failed

    mov cx, 4
    mov ah, 0x3f
    int 0x21
    jc failed

    mov cx, 4
    mov ah, 0x3f
    int 0x21
    jc failed

    mov ah, 0x3e
    int 0x21
    jc failed

    mov dx, offset second_name
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    jc failed
    mov bx, ax

    mov dx, offset second_payload
    mov cx, 3
    mov ah, 0x40
    int 0x21
    jc failed

    mov ah, 0x3e
    int 0x21
    jc failed

    mov ax, 0x4c00
    int 0x21

failed:
    mov ax, 0x4cff
    int 0x21

first_name:
    .asciz "FIRST.DAT"
missing_name:
    .asciz "MISSING.DAT"
second_name:
    .asciz "SECOND.DAT"
first_payload:
    .ascii "ABCDE"
second_payload:
    .ascii "XYZ"
read_buffer:
    .space 8
