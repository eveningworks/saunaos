; hello.asm -- w16's first test program: one window that says hello.
; The classic Win16 shape, by hand: task start-up, a window class, a
; window, the message loop, and a window procedure that paints text and
; quits when the window is destroyed. Build: tools/gen_ne.py.
;
;w16: module HELLO
;w16: description w16 test program: one window that says hello
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY  0x0002
%define WM_PAINT    0x000F
%define CW_USEDEFAULT 0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW 5
%define IDC_ARROW   32512

; ---------------------------------------------------------------- code
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
    push word IDC_ARROW
    W16_CALL "USER", 173            ; LoadCursor(NULL, IDC_ARROW)
    mov [wc_hCursor], ax
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass(&wc)
    or ax, ax
    jz fail

    push ds                         ; CreateWindow(class, title, style,
    push word classname             ;   x, y, w, h, parent, menu,
    push ds                         ;   hInstance, lpParam)
    push word title
    push word WS_OVERLAPPEDWINDOW >> 16
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
    push word 400
    push word 200
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
    mov al, [msg_wParam]
    mov ah, 0x4C
    int 0x21                        ; exit with wParam's low byte

fail:
    mov ax, 0x4C01
    int 0x21

; LONG FAR PASCAL WndProc(HWND, UINT, WPARAM, LPARAM): [bp+14] hwnd,
; [bp+12] msg, [bp+10] wParam, [bp+8]:[bp+6] lParam; pops its 10 bytes.
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
    push ax                         ; TextOut(hdc, x, y, str, len)
    push word 16
    push word 16
    push ds
    push word hello_text
    push word hello_len
    W16_CALL "GDI", 33
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

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0                   ; the task's reserved words; InitTask fills them

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0

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

msg:                                ; MSG
            dw 0, 0                 ; hwnd, message
msg_wParam: dw 0
            dd 0, 0                 ; lParam, time
            dw 0, 0                 ; pt

ps:         times 32 db 0           ; PAINTSTRUCT

classname:  db "HelloClass", 0
title:      db "Hello (Win16)", 0
hello_text: db "Hello from a Win16 program, running in /bin/w16."
hello_len   equ $ - hello_text
