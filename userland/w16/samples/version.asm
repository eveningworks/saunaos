; version.asm -- what a program is told about the Windows it runs on:
; GetVersion() and GetWinFlags(), in hex, to VERTEST.INI (drive C:'s
; WINDOWS folder, where Windows' own profile calls put it):
;   [VerTest] Version=0A03 Dos=0500 Flags=00000413
; Run on real Windows 3.11 for Workgroups and on w16, it is the check of
; what w16's "Version told" setting should answer. No window.
; Build: tools/gen_ne.py.
;
;w16: module VERTEST
;w16: description Version test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz fail

    W16_CALL "KERNEL", 3            ; GetVersion: AX Windows, DX DOS
    mov [version], ax
    mov [dosver], dx
    W16_CALL "KERNEL", 132          ; GetWinFlags: DX:AX
    mov [flags], ax
    mov [flags + 2], dx

    push ds
    pop es
    mov di, version_text
    mov ax, [version]
    call hex16
    mov byte [di], 0
    mov di, dos_text
    mov ax, [dosver]
    call hex16
    mov byte [di], 0
    mov di, flags_text
    mov ax, [flags + 2]
    call hex16
    mov ax, [flags]
    call hex16
    mov byte [di], 0

    mov si, keys
.write:
    lodsw                           ; a key's name, then its text
    or ax, ax
    jz .written
    mov dx, ax
    lodsw
    push si                         ; kept across the call, under its arguments
    push ds                         ; WritePrivateProfileString("VerTest", key, text, "VERTEST.INI")
    push word ini_app
    push ds
    push dx
    push ds
    push ax
    push ds
    push word ini_file
    W16_CALL "KERNEL", 129
    pop si
    jmp .write
.written:
    mov ax, 0x4C00
    int 0x21
fail:
    mov ax, 0x4C01
    int 0x21

; AX as four hex digits at ES:DI.
hex16:
    push ax
    mov al, ah
    call hex8
    pop ax
hex8:
    push ax
    shr al, 4
    call .digit
    pop ax
.digit:
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .put
    add al, 'A' - '9' - 1
.put:
    stosb
    ret

W16_ENTRY start

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            times 16 db 0           ; the Windows header words a task keeps
version:    dw 0
dosver:     dw 0
flags:      dd 0

keys:       dw key_version, version_text, key_dos, dos_text
            dw key_flags, flags_text, 0
key_version: db "Version", 0
key_dos:    db "Dos", 0
key_flags:  db "Flags", 0
version_text: times 6 db 0
dos_text:   times 6 db 0
flags_text: times 10 db 0
ini_app:    db "VerTest", 0
ini_file:   db "VERTEST.INI", 0
