; cursor.asm -- w16's test program for a program's OWN cursors (icon16.c's
; w16_cursor_handle(), toywm's WIN_REQ_CURSOR_IMAGE). Its window's left
; half is black, its right half white, and its class cursor is cursor 1:
; a crosshair of INVERTING pixels, hotspot at its centre (15, 15), with a
; black block at (2..4, 2..4) and a white one at (26..28, 26..28) -- all
; four of a Windows cursor's pixel kinds. So over the white half the arms
; are black, over the black half white, and the blocks the same on both.
;
;   a left click:  SetClassWord(hwnd, GCW_HCURSOR, cursor 2) -- an
;                  inverting square, from the next move on
;   a right click: SetCursor(NULL) -- no pointer until the next move
;   a key:         GlobalLock(cursor 1), its black block's XOR bits set --
;                  so it turns white -- and SetCursor(cursor 1), as
;                  Paintbrush draws its brush into a cursor
;
; Build: tools/gen_ne.py.
;
;w16: module CURSOR
;w16: description Own cursors test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: cursor 1 15 15
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ..###..........~
;w16: crow 1 ..###..........~
;w16: crow 1 ..###..........~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ~~~~~~~~~~~~~~~.~~~~~~~~~~~~~~~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: crow 1 ...............~..........ooo
;w16: crow 1 ...............~..........ooo
;w16: crow 1 ...............~..........ooo
;w16: crow 1 ...............~
;w16: crow 1 ...............~
;w16: cursor 2 10 10
;w16: crow 2 ~~~~~~~~~~~~~~~~~~~~~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~...................~
;w16: crow 2 ~~~~~~~~~~~~~~~~~~~~~

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F
%define WM_KEYDOWN     0x0100
%define WM_LBUTTONDOWN 0x0201
%define WM_RBUTTONDOWN 0x0204
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW   5
%define BLACK_BRUSH    4
%define GCW_HCURSOR    -12

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
    push word [hInstance]
    push word 0
    push word 1
    W16_CALL "USER", 173            ; LoadCursor(hInstance, MAKEINTRESOURCE(1))
    mov [wc_hCursor], ax
    push word [hInstance]
    push word 0
    push word 2
    W16_CALL "USER", 173            ; LoadCursor(hInstance, MAKEINTRESOURCE(2))
    mov [hSquare], ax
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
    cmp ax, WM_LBUTTONDOWN
    je .left
    cmp ax, WM_RBUTTONDOWN
    je .right
    cmp ax, WM_KEYDOWN
    je .key
    cmp ax, WM_DESTROY
    je .destroy
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret
.left:
    push word [bp+14]
    push word GCW_HCURSOR
    push word [hSquare]
    W16_CALL "USER", 130            ; SetClassWord(hwnd, GCW_HCURSOR, hSquare)
    jmp .zero
.right:
    push word 0
    W16_CALL "USER", 69             ; SetCursor(NULL)
    jmp .zero
.key:
    push word [wc_hCursor]
    W16_CALL "KERNEL", 18           ; GlobalLock(cursor 1): DX:AX its CURSORICONINFO
    mov es, dx
    mov bx, ax
    or byte [es:bx+12+128+2*4], 0x38   ; XOR rows 2..4, columns 2..4 (after the 12-byte
    or byte [es:bx+12+128+3*4], 0x38   ; header and the 32 rows of AND)
    or byte [es:bx+12+128+4*4], 0x38
    push word [wc_hCursor]
    W16_CALL "KERNEL", 19           ; GlobalUnlock
    push word [wc_hCursor]
    W16_CALL "USER", 69             ; SetCursor(cursor 1)
    jmp .zero
.paint:
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 39             ; BeginPaint(hwnd, &ps)
    mov [hdc], ax
    push word BLACK_BRUSH
    W16_CALL "GDI", 87              ; GetStockObject(BLACK_BRUSH)
    push word [hdc]
    push ds
    push word left_half
    push ax
    W16_CALL "USER", 81             ; FillRect(hdc, &left_half, black)
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint(hwnd, &ps)
.zero:
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
hSquare:    dw 0

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

left_half:  dw 0, 0, 140, 400       ; RECT: the client area's left 140 pixels

msg:        dw 0, 0, 0              ; MSG
            dd 0, 0
            dw 0, 0

ps:         times 32 db 0           ; PAINTSTRUCT

classname:  db "CursorClass", 0
title:      db "Cursors (Win16)", 0
