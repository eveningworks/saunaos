; fpu.asm -- w16's test program for the x87 and WIN87EM (cpu16_x87.c,
; win87em16.c): no window, just the coprocessor, as a C runtime drives it.
; _fpMath initialises (BX=0: control word 1332h, invalid, zero divide and
; overflow unmasked) and takes a handler (BX=3); then
;
;   1/0                       an unmasked zero divide    -> the handler, AL 83h
;   sqrt(-1)                  an unmasked invalid        -> AL 81h
;   nine loads                a stack overflow           -> AL 8Ah
;   1/0, everything masked    no call; BX=8 reports the zero divide
;   sqrt(2) * 1e9, FISTP      1414213562, exactly
;
; Each exception reaches the handler at the FWAIT after it, once: the
; handler returns, the FWAIT runs again, and the exception is gone. What it
; saw goes to FPUTEST.INI (drive C:'s WINDOWS folder):
;   [FpuTest] Codes=83818A Calls=0003 Status=0004 Root=544B2FBA
; and the program ends. UNDER QEMU's TCG the stack overflow never happens
; -- its x87 has no stack fault -- so there it is Codes=8381 Calls=0002.
; Build: tools/gen_ne.py.
;
;w16: module FPUTEST
;w16: description FPU test
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

    xor bx, bx                      ; _fpMath BX=0: start using the coprocessor
    W16_CALL "WIN87EM", 1
    jc fail
    mov ax, handler                 ; BX=3: DX:AX, the exception handler
    mov dx, cs
    mov bx, 3
    W16_CALL "WIN87EM", 1

    fld1                            ; 1/0: ST(1) / ST(0), unmasked
    fldz
    fdivp st1, st0
    fwait                           ; the handler, AL 83h, then this again
    mov bx, 1                       ; BX=1: initialise again
    W16_CALL "WIN87EM", 1

    fld qword [minus_one]           ; sqrt(-1): invalid
    fsqrt
    fwait
    mov bx, 1
    W16_CALL "WIN87EM", 1

    mov cx, 9                       ; the ninth load has no register left
.load:
    fld1
    loop .load
    fwait
    mov bx, 1
    W16_CALL "WIN87EM", 1

    mov ax, 0x133F                  ; BX=4: everything masked
    mov bx, 4
    W16_CALL "WIN87EM", 1
    fld1
    fldz
    fdivp st1, st0                  ; infinity, no handler
    fstp st0
    fwait
    mov bx, 8                       ; BX=8: the exceptions so far
    W16_CALL "WIN87EM", 1
    mov [status], ax

    fld qword [two]                 ; sqrt(2) * 1e9, to an integer
    fsqrt
    fmul qword [billion]
    fistp dword [root]
    fwait

    ; [FpuTest] Codes= Calls= Status= Root=, in hex
    push ds
    pop es
    mov di, codes_text
    xor bx, bx
.codes:
    cmp bx, [ncodes]
    jae .codes_done
    cmp bx, 8
    jae .codes_done
    mov al, [codes + bx]
    call hex8
    inc bx
    jmp .codes
.codes_done:
    mov byte [di], 0
    mov di, calls_text
    mov ax, [ncodes]
    call hex16
    mov byte [di], 0
    mov di, status_text
    mov ax, [status]
    call hex16
    mov byte [di], 0
    mov di, root_text
    mov ax, [root + 2]
    call hex16
    mov ax, [root]
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
    push ds                         ; WritePrivateProfileString("FpuTest", key, text, "FPUTEST.INI")
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

; The runtime's handler, as WIN87EM calls it: AL says which exception.
handler:
    push bx
    mov bx, [ncodes]
    cmp bx, 8
    jae .full
    mov [codes + bx], al
.full:
    inc word [ncodes]
    pop bx
    retf

; AL as two hex digits at ES:DI; AX as four.
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
minus_one:  dq -1.0
two:        dq 2.0
billion:    dq 1.0e9
root:       dd 0
status:     dw 0
ncodes:     dw 0
codes:      times 8 db 0

keys:       dw key_codes, codes_text, key_calls, calls_text
            dw key_status, status_text, key_root, root_text, 0
key_codes:  db "Codes", 0
key_calls:  db "Calls", 0
key_status: db "Status", 0
key_root:   db "Root", 0
codes_text: times 20 db 0
calls_text: times 6 db 0
status_text: times 6 db 0
root_text:  times 10 db 0
ini_app:    db "FpuTest", 0
ini_file:   db "FPUTEST.INI", 0
