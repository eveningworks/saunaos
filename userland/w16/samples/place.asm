; place.asm -- a popup PLACED by its program, as DK's encyclopaedia places its panels: a 120 x 80
; WS_POPUP with a caption at 40, 30 of the screen, over a 400 x 300 main window, its client area red
; with a 20 x 20 green square at the corner. A click in it does what TIEDE does to keep a panel where
; it is: GetWindowRect, then MoveWindow to that rectangle -- 10 pixels to the right -- so a
; GetWindowRect answering 0,0 sends it to the corner, and one that missed a drag sends it back. A key
; destroys it. tools/tests/gui/w16_gui_test.py checks where toywm puts it, that it follows the main window, and
; the click's move before and after a drag of its caption.
; Build: tools/gen/gen_ne.py.
;
;w16: module PLACE
;w16: description Place test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F
%define WM_KEYDOWN     0x0100
%define WM_LBUTTONDOWN 0x0201
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define WS_POPUP_CAPTION    0x90C0      ; WS_POPUP | WS_VISIBLE | WS_CAPTION, the high word
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
    W16_CALL "USER", 57             ; RegisterClass: the main window's
    or ax, ax
    jz fail
    push word 0x0000                ; CreateSolidBrush(RGB(255,0,0)): the popup's background
    push word 0x00FF
    W16_CALL "GDI", 66
    mov [wc_hbr], ax
    mov word [wc_name], popclass
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass: the popup's
    or ax, ax
    jz fail

    push ds
    push word mainclass
    push ds
    push word title
    push word WS_OVERLAPPEDWINDOW >> 16
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
    push word 400
    push word 300
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: the main window
    or ax, ax
    jz fail
    mov [hwnd], ax
    push ax
    push word [nCmdShow]
    W16_CALL "USER", 42             ; ShowWindow
    push word [hwnd]
    W16_CALL "USER", 124            ; UpdateWindow

    push ds
    push word popclass
    push ds
    push word empty
    push word WS_POPUP_CAPTION
    push word 0
    push word 40                    ; at 40, 30
    push word 30
    push word 120                   ; 120 x 80
    push word 80
    push word [hwnd]                ; its owner
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: the popup
    mov [hpopup], ax

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
    W16_CALL "USER", 113            ; TranslateMessage
    push ds
    push word msg
    W16_CALL "USER", 114            ; DispatchMessage
    jmp msgloop

quit:
    mov al, [msg_wParam]
    mov ah, 0x4C
    int 0x21

fail:
    mov ax, 0x4C01
    int 0x21

; The one procedure of both classes, (hwnd, msg, wParam, lParam), Pascal, DS from AX.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+14]
    cmp ax, [hpopup]
    jne .main
    mov ax, [bp+12]
    cmp ax, WM_LBUTTONDOWN
    je .nudge
    cmp ax, WM_KEYDOWN
    je .gone
    cmp ax, WM_PAINT
    je .paint
    jmp .default
.nudge:
    push word [hpopup]
    push ds
    push word rect
    W16_CALL "USER", 32             ; GetWindowRect(hpopup, &rect)
    push word [hpopup]              ; MoveWindow(hpopup, left + 10, top, right - left, bottom - top, TRUE)
    mov ax, [rect]
    add ax, 10
    push ax
    push word [rect + 2]
    mov ax, [rect + 4]
    sub ax, [rect]
    push ax
    mov ax, [rect + 6]
    sub ax, [rect + 2]
    push ax
    push word 1
    W16_CALL "USER", 56
    jmp .zero
.gone:
    push word [hpopup]
    W16_CALL "USER", 53             ; DestroyWindow
    mov word [hpopup], 0
    jmp .zero
.paint:
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 39             ; BeginPaint
    mov [hdc], ax
    push word 0x0000                ; CreateSolidBrush(RGB(0,255,0)): the COLORREF's high word first
    push word 0xFF00
    W16_CALL "GDI", 66
    mov [hgreen], ax
    push word [hdc]
    push ax
    W16_CALL "GDI", 45              ; SelectObject
    mov [oldbrush], ax
    push word [hdc]                 ; PatBlt(hdc, 0, 0, 20, 20, PATCOPY)
    push word 0
    push word 0
    push word 20
    push word 20
    push word 0x00F0
    push word 0x0021
    W16_CALL "GDI", 29
    push word [hdc]
    push word [oldbrush]
    W16_CALL "GDI", 45
    push word [hgreen]
    W16_CALL "GDI", 69
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint
    jmp .zero
.main:
    cmp word [bp+12], WM_DESTROY
    jne .default
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
    jmp .zero
.default:
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret
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

    times 16 db 0

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
hpopup:     dw 0
hdc:        dw 0
hgreen:     dw 0
oldbrush:   dw 0
rect:       dw 0, 0, 0, 0

wc:
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
wc_hbr:     dw COLOR_WINDOW + 1
            dd 0
wc_name:    W16_FARPTR mainclass, 2

msg:
            dw 0, 0
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

ps:         times 32 db 0

mainclass:  db "PlaceMain", 0
popclass:   db "PlaceTest", 0
title:      db "Place test", 0
empty:      db 0
