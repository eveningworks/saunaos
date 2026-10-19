; fault.asm -- w16's test program for a program that goes wrong: a click
; in its window loads the null selector and writes through it (a general
; protection fault), a right click beeps (MessageBeep: a stand-in, silent,
; so the launcher has a "partly" call to show) and calls ExitWindows, which
; w16 does not implement. Either ends in the Application Error box and a
; crash report.
; Build: tools/gen_ne.py.
;
;w16: module FAULT
;w16: description Fault test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F
%define WM_LBUTTONDOWN 0x0201
%define WM_RBUTTONDOWN 0x0204
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW   5
%define IDC_ARROW      32512

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz fail
    mov [hInstance], di
    mov [nCmdShow], dx
    push word 0
    W16_CALL "KERNEL", 30           ; WaitEvent(0)
    push word [hInstance]
    W16_CALL "USER", 5              ; InitApp(hInstance)

    mov ax, [hInstance]
    mov [wc_hInstance], ax
    push word 0
    push word 0
    push word IDC_ARROW
    W16_CALL "USER", 173            ; LoadCursor(NULL, IDC_ARROW)
    mov [wc_hCursor], ax
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass(&wc)
    or ax, ax
    jz fail

    push ds
    push word classname
    push ds
    push word title
    push word WS_OVERLAPPEDWINDOW >> 16
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
    push word 440
    push word 200
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow
    or ax, ax
    jz fail
    mov [hwnd], ax

    push ax
    push word [nCmdShow]
    W16_CALL "USER", 42             ; ShowWindow(hwnd, nCmdShow)
    push word [hwnd]
    W16_CALL "USER", 124            ; UpdateWindow(hwnd)

msgloop:
    push ds
    push word msg
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 108            ; GetMessage(&msg, 0, 0, 0)
    or ax, ax
    jz quit
    push ds
    push word msg
    W16_CALL "USER", 113            ; TranslateMessage(&msg)
    push ds
    push word msg
    W16_CALL "USER", 114            ; DispatchMessage(&msg)
    jmp msgloop

quit:
    mov al, [msg_wParam]
    mov ah, 0x4C
    int 0x21

fail:
    mov ax, 0x4C01
    int 0x21

; LONG FAR PASCAL WndProc(HWND, UINT, WPARAM, LPARAM), as hello.asm's.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_PAINT
    je .paint
    cmp ax, WM_LBUTTONDOWN
    je .gpf
    cmp ax, WM_RBUTTONDOWN
    je .help
    cmp ax, WM_DESTROY
    je .destroy
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret
.paint:
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 39             ; BeginPaint(hwnd, &ps)
    mov [hdc], ax
    push ax                         ; TextOut(hdc, 16, 16, line1)
    push word 16
    push word 16
    push ds
    push word line1
    push word line1_len
    W16_CALL "GDI", 33
    push word [hdc]                 ; TextOut(hdc, 16, 40, line2)
    push word 16
    push word 40
    push ds
    push word line2
    push word line2_len
    W16_CALL "GDI", 33
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint(hwnd, &ps)
    xor ax, ax
    xor dx, dx
    jmp .ret
.gpf:
    xor ax, ax
    mov es, ax                      ; the null selector: legal to load...
    mov [es:0], ax                  ; ...and a general protection fault to use
    jmp .zero
.help:
    push word 0
    W16_CALL "USER", 104            ; MessageBeep(0)
    push word 0                     ; ExitWindows(0, 0)
    push word 0
    push word 0
    W16_CALL "USER", 7
    jmp .zero
.destroy:
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage(0)
.zero:
    xor ax, ax
    xor dx, dx
.ret:
    pop ds
    pop bp
    retf 10

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0                   ; the task's reserved words

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
hdc:        dw 0

wc:                                 ; WNDCLASS
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
            dw COLOR_WINDOW + 1
            dd 0
            W16_FARPTR classname, 2

msg:
            dw 0, 0
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

ps:         times 32 db 0

classname:  db "FaultClass", 0
title:      db "Fault (Win16)", 0
line1:      db "Click: a general protection fault."
line1_len   equ $ - line1
line2:      db "Right-click: ExitWindows, which w16 does not have."
line2_len   equ $ - line2
