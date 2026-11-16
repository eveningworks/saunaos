; busydraw.asm -- a program that draws and then works without asking for a message, as WinBench's disk
; test labels each phase with GetDC and then reads and writes for minutes. tools/tests/gui/
; w16_gui_test.py's `busy_draw` checks by pixel:
;
;   - a window 300 x 200 ("Busy draw test"), shown and updated, and its messages taken until there are
;     none (PeekMessage): what it drew so far is on the screen, as WinBench's window is before a test
;   - GetDC, a black 100 x 50 PatBlt at client (20, 20), ReleaseDC -- then 8 seconds of GetTickCount
;     and nothing else: 3.1 drew straight onto the screen, so the black shows during those 8 seconds
;   - then the message loop
;
; Build: tools/gen/gen_ne.py.
;
;w16: module BUSYDRAW
;w16: description Busy draw test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY  0x0002
%define CW_USEDEFAULT 0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW 5
%define IDC_ARROW   32512
%define BLACKNESS_HI 0x0000
%define BLACKNESS_LO 0x0042

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
    W16_CALL "USER", 5              ; InitApp
    mov ax, [hInstance]
    mov [wc_hInstance], ax
    push word 0
    push word 0
    push word IDC_ARROW
    W16_CALL "USER", 173            ; LoadCursor
    mov [wc_hCursor], ax
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass
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
    push word 300
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
    W16_CALL "USER", 42             ; ShowWindow
    push word [hwnd]
    W16_CALL "USER", 124            ; UpdateWindow
drain:
    push ds                         ; PeekMessage(&msg, 0, 0, 0, PM_REMOVE) until there is none
    push word msg
    push word 0
    push word 0
    push word 0
    push word 1
    W16_CALL "USER", 109
    or ax, ax
    jz drained
    push ds
    push word msg
    W16_CALL "USER", 114            ; DispatchMessage
    jmp drain
drained:

    push word [hwnd]
    W16_CALL "USER", 66             ; GetDC
    mov [hdc], ax
    push ax                         ; PatBlt(hdc, 20, 20, 100, 50, BLACKNESS)
    push word 20
    push word 20
    push word 100
    push word 50
    push word BLACKNESS_HI
    push word BLACKNESS_LO
    W16_CALL "GDI", 29
    push word [hwnd]
    push word [hdc]
    W16_CALL "USER", 68             ; ReleaseDC

    W16_CALL "USER", 13             ; GetTickCount -> DX:AX
    mov [t0], ax
    mov [t0 + 2], dx
busy:
    W16_CALL "USER", 13
    sub ax, [t0]
    sbb dx, [t0 + 2]
    or dx, dx
    jnz msgloop
    cmp ax, 8000
    jb busy

msgloop:
    push ds
    push word msg
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 108            ; GetMessage
    or ax, ax
    jz quit
    push ds
    push word msg
    W16_CALL "USER", 114            ; DispatchMessage
    jmp msgloop
quit:
    mov ax, 0x4C00
    int 0x21
fail:
    mov ax, 0x4C01
    int 0x21

; LONG FAR PASCAL WndProc: [bp+14] hwnd, [bp+12] msg, [bp+10] wParam, [bp+8]:[bp+6] lParam.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    cmp word [bp+12], WM_DESTROY
    je .destroy
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret
.destroy:
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
    xor ax, ax
    xor dx, dx
.ret:
    pop ds
    pop bp
    retf 10

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc

W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
hdc:        dw 0
t0:         dd 0

wc:
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
            dw 0
            dd 0, 0
            dw 0, 0

classname:  db "BusyDrawClass", 0
title:      db "Busy draw test", 0
