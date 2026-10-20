; shapes.asm -- w16's test program for GDI's curves: rounded rectangles,
; ellipses, arcs, pies and chords (gdi16.c), drawn in the stock black pen
; with the light grey brush, in client coordinates:
;
;   RoundRect(10, 10, 70, 50, 20, 16)
;   RoundRect(80, 10, 141, 51, 9, 7)
;   Ellipse(150, 10, 210, 50)
;   Ellipse(220, 10, 231, 17)
;   Arc(10, 60, 70, 110, 70, 85, 40, 60)
;   Pie(80, 60, 140, 110, 140, 60, 80, 110)
;   Chord(150, 60, 210, 110, 210, 85, 150, 85)
;   Pie(220, 60, 280, 110, 250, 60, 280, 85)
;   Arc(10, 120, 71, 161, 10, 140, 10, 140)
;   Chord(80, 120, 140, 160, 80, 120, 140, 160)
;   RoundRect(150, 120, 210, 160, 60, 40)
;   Pie(220, 120, 280, 160, 280, 140, 280, 141)
;
; -- every sweep counterclockwise from the first radial point to the
; second, the same point twice a whole ellipse. The window is the oracle
; for a comparison with real Windows 3.11, which draws it the same way.
; Build: tools/gen_ne.py.
;
;w16: module SHAPES
;w16: description GDI curves test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY  0x0002
%define WM_PAINT    0x000F
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW 5
%define IDC_ARROW   32512
%define LTGRAY_BRUSH 1
%define BLACK_PEN   7

W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask: AX=1, DI=hInstance, DX=nCmdShow
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

    push ds                         ; CreateWindow(class, title, style, 20, 20, 300, 210, ...)
    push word classname
    push ds
    push word title
    push word WS_OVERLAPPEDWINDOW >> 16
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word 20
    push word 20
    push word 300
    push word 210
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41
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
    mov ax, 0x4C00
    int 0x21

fail:
    mov ax, 0x4C01
    int 0x21

wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax                      ; AX = our DGROUP, as an instance thunk sets it
    mov ax, [bp+12]
    cmp ax, WM_PAINT
    je .paint
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
    push word BLACK_PEN
    W16_CALL "GDI", 87              ; GetStockObject
    push word [hdc]
    push ax
    W16_CALL "GDI", 45              ; SelectObject
    push word LTGRAY_BRUSH
    W16_CALL "GDI", 87
    push word [hdc]
    push ax
    W16_CALL "GDI", 45
    push word [hdc]                 ; RoundRect(10, 10, 70, 50, 20, 16)
    push word 10
    push word 10
    push word 70
    push word 50
    push word 20
    push word 16
    W16_CALL "GDI", 28
    push word [hdc]                 ; RoundRect(80, 10, 141, 51, 9, 7)
    push word 80
    push word 10
    push word 141
    push word 51
    push word 9
    push word 7
    W16_CALL "GDI", 28
    push word [hdc]                 ; Ellipse(150, 10, 210, 50)
    push word 150
    push word 10
    push word 210
    push word 50
    W16_CALL "GDI", 24
    push word [hdc]                 ; Ellipse(220, 10, 231, 17)
    push word 220
    push word 10
    push word 231
    push word 17
    W16_CALL "GDI", 24
    push word [hdc]                 ; Arc(10, 60, 70, 110, 70, 85, 40, 60)
    push word 10
    push word 60
    push word 70
    push word 110
    push word 70
    push word 85
    push word 40
    push word 60
    W16_CALL "GDI", 23
    push word [hdc]                 ; Pie(80, 60, 140, 110, 140, 60, 80, 110)
    push word 80
    push word 60
    push word 140
    push word 110
    push word 140
    push word 60
    push word 80
    push word 110
    W16_CALL "GDI", 26
    push word [hdc]                 ; Chord(150, 60, 210, 110, 210, 85, 150, 85)
    push word 150
    push word 60
    push word 210
    push word 110
    push word 210
    push word 85
    push word 150
    push word 85
    W16_CALL "GDI", 348
    push word [hdc]                 ; Pie(220, 60, 280, 110, 250, 60, 280, 85)
    push word 220
    push word 60
    push word 280
    push word 110
    push word 250
    push word 60
    push word 280
    push word 85
    W16_CALL "GDI", 26
    push word [hdc]                 ; Arc(10, 120, 71, 161, 10, 140, 10, 140)
    push word 10
    push word 120
    push word 71
    push word 161
    push word 10
    push word 140
    push word 10
    push word 140
    W16_CALL "GDI", 23
    push word [hdc]                 ; Chord(80, 120, 140, 160, 80, 120, 140, 160)
    push word 80
    push word 120
    push word 140
    push word 160
    push word 80
    push word 120
    push word 140
    push word 160
    W16_CALL "GDI", 348
    push word [hdc]                 ; RoundRect(150, 120, 210, 160, 60, 40)
    push word 150
    push word 120
    push word 210
    push word 160
    push word 60
    push word 40
    W16_CALL "GDI", 28
    push word [hdc]                 ; Pie(220, 120, 280, 160, 280, 140, 280, 141)
    push word 220
    push word 120
    push word 280
    push word 160
    push word 280
    push word 140
    push word 280
    push word 141
    W16_CALL "GDI", 26
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint(hwnd, &ps)
    xor ax, ax
    xor dx, dx
    jmp .ret
.destroy:
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage(0)
    xor ax, ax
    xor dx, dx
.ret:
    pop ds
    pop bp
    retf 10

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc

W16_SEGMENT 2

    times 16 db 0                   ; the task's reserved words; InitTask fills them

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
hdc:        dw 0

wc:                                 ; WNDCLASS
            dw 0                    ; style
            W16_FARPTR wndproc, 1   ; lpfnWndProc
            dw 0, 0                 ; cbClsExtra, cbWndExtra
wc_hInstance: dw 0
            dw 0                    ; hIcon
wc_hCursor: dw 0
            dw COLOR_WINDOW + 1     ; hbrBackground
            dd 0                    ; lpszMenuName
            W16_FARPTR classname, 2 ; lpszClassName

msg:        dw 0, 0, 0              ; MSG
            dd 0, 0
            dw 0, 0

ps:         times 32 db 0           ; PAINTSTRUCT

classname:  db "ShapesClass", 0
title:      db "Shapes (Win16)", 0
