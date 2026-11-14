; fullscr.asm -- a main window as Sierra's games and Setup make one: a WS_POPUP with no caption,
; WS_MAXIMIZE and CW_USEDEFAULT, shown SW_SHOWMAXIMIZED, so 3.1 gave it the whole 640 x 480 screen.
; tools/tests/gui/w16_gui_test.py's `fullscreen` checks, by pixel and by FULLSCR.INI's [Full]:
;
;   - the window fills SaunaOS's screen, its 640 x 480 scaled by the largest quarter step that fits
;     and centred on black: blue (its class brush), a green 40 x 40 square at its (0,0) and a yellow
;     one at its (600,440), so both corners of the picture can be found
;   - LoadCursor(hInst, "NOSUCH") is NULL (Cursor=0): the module has no such cursor, and 3.1 answered
;     none, which SetCursor then hides -- Sierra's SCI draws its own pointer so
;   - over it SaunaOS shows no pointer: its class cursor is the arrow, but it answers WM_SETCURSOR
;     with SetCursor(NULL), as SCI does -- 3.1 asked the window before applying a class cursor
;   - a click lands where it was made in the picture: WM_LBUTTONDOWN's point (ClickX=, ClickY=)
;   - then DIALOG 100, whose procedure answers WM_CTLCOLOR with LTGRAY_BRUSH for the dialog itself
;     (CTLCOLOR_DLG) and its static, and sets the static's text red: 3.1's DefDlgProc erased with
;     that brush, and the static drew in that colour (Sierra's Setup is grey, its link red)
;   - a key ends it.
;
; Build: tools/gen/gen_ne.py.
;
;w16: module FULLSCR
;w16: description Fullscreen test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: dialog 100 0 0 160 60 0x80c80080 "Colours"
;w16: control 100 STATIC 101 10 10 140 20 0x50000000 "Red on grey"
;w16: control 100 BUTTON 1 58 38 44 14 0x50010001 "OK"

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F
%define WM_KEYDOWN     0x0100
%define WM_LBUTTONDOWN 0x0201
%define WM_COMMAND     0x0111
%define WM_INITDIALOG  0x0110
%define WM_CTLCOLOR    0x0019
%define WM_SETCURSOR   0x0020
%define CW_USEDEFAULT  0x8000
%define WS_POPUP_MAX   0x8100          ; WS_POPUP | WS_MAXIMIZE, the high word
%define SW_SHOWMAXIMIZED 3

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz fail
    mov [hInstance], di
    push word 0
    W16_CALL "KERNEL", 30           ; WaitEvent(0)
    push word [hInstance]
    W16_CALL "USER", 5              ; InitApp

    mov ax, [hInstance]
    mov [wc_hInstance], ax
    push word 0x00FF                ; CreateSolidBrush(RGB(0,0,255)): the window's blue
    push word 0x0000
    W16_CALL "GDI", 66
    mov [wc_hbr], ax
    push word 0                     ; LoadCursor(NULL, IDC_ARROW): the class cursor
    push word 0
    push word 0x7F00
    W16_CALL "USER", 173
    mov [wc_hcur], ax
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass
    or ax, ax
    jz fail

    push word [hInstance]           ; LoadCursor(hInst, "NOSUCH"): none in this module
    push ds
    push word nosuch
    W16_CALL "USER", 173
    mov di, key_cursor
    call save_ax

    push ds
    push word mainclass
    push ds
    push word title
    push word WS_POPUP_MAX
    push word 0
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
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
    push word SW_SHOWMAXIMIZED
    W16_CALL "USER", 42             ; ShowWindow
    push word [hwnd]
    W16_CALL "USER", 124            ; UpdateWindow

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

; [Full] <key>=AX, as a decimal. DI = the key's offset in DGROUP.
save_ax:
    push ax                         ; wsprintf(numbuf, "%d", ax) -- cdecl, the caller pops
    push ds
    push word fmt_d
    push ds
    push word numbuf
    W16_CALL "USER", 420
    add sp, 10
    push ds                         ; WritePrivateProfileString("Full", key, numbuf, "FULLSCR.INI")
    push word ini_app
    push ds
    push di
    push ds
    push word numbuf
    push ds
    push word ini_file
    W16_CALL "KERNEL", 129
    ret

; A square of `brush` (CX) at (SI, DI), 40 x 40, on hdc.
square:
    push word [hdc]
    push cx
    W16_CALL "GDI", 45              ; SelectObject
    mov [oldbrush], ax
    push word [hdc]                 ; PatBlt(hdc, x, y, 40, 40, PATCOPY)
    push si
    push di
    push word 40
    push word 40
    push word 0x00F0
    push word 0x0021
    W16_CALL "GDI", 29
    push word [hdc]
    push word [oldbrush]
    W16_CALL "GDI", 45
    ret

; LRESULT FAR PASCAL WndProc(hwnd, msg, wParam, lParam), DS from AX.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_PAINT
    je .paint
    cmp ax, WM_LBUTTONDOWN
    je .click
    cmp ax, WM_KEYDOWN
    je .key
    cmp ax, WM_SETCURSOR
    je .setcursor
    cmp ax, WM_DESTROY
    je .destroy
    jmp .default
.paint:
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 39             ; BeginPaint
    mov [hdc], ax
    push word 0x0000                ; CreateSolidBrush(RGB(0,255,0))
    push word 0xFF00
    W16_CALL "GDI", 66
    mov [hgreen], ax
    push word 0x0000                ; CreateSolidBrush(RGB(255,255,0))
    push word 0xFFFF
    W16_CALL "GDI", 66
    mov [hyellow], ax
    mov cx, [hgreen]
    xor si, si
    xor di, di
    call square
    mov cx, [hyellow]
    mov si, 600
    mov di, 440
    call square
    push word [hgreen]
    W16_CALL "GDI", 69              ; DeleteObject
    push word [hyellow]
    W16_CALL "GDI", 69
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint
    jmp .zero
.click:
    mov ax, [bp+6]                  ; lParam: x low, y high
    mov di, key_clickx
    call save_ax
    mov ax, [bp+8]
    mov di, key_clicky
    call save_ax
    push word [hInstance]           ; DialogBox(hInst, MAKEINTRESOURCE(100), hwnd, DlgProc)
    push word 0
    push word 100
    push word [bp+14]
    push word [dlgproc_ptr + 2]
    push word [dlgproc_ptr]
    W16_CALL "USER", 87
    jmp .zero
.key:
    push word [bp+14]
    W16_CALL "USER", 53             ; DestroyWindow
    jmp .zero
.setcursor:
    push word 0
    W16_CALL "USER", 69             ; SetCursor(NULL): no pointer over it, and TRUE -- handled
    mov ax, 1
    xor dx, dx
    jmp .ret
.destroy:
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

; BOOL FAR PASCAL DlgProc(hwnd, msg, wParam, lParam): grey for itself and its static, the static's
; text red; OK ends it.
dlgproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_INITDIALOG
    je .yes
    cmp ax, WM_CTLCOLOR
    je .color
    cmp ax, WM_COMMAND
    je .command
.no:
    xor ax, ax
    jmp .ret
.yes:
    mov ax, 1
    jmp .ret
.color:
    cmp word [bp+8], 6              ; CTLCOLOR_STATIC: its text red
    jne .brush
    push word [bp+10]               ; SetTextColor(hdc, RGB(255,0,0))
    push word 0x0000
    push word 0x00FF
    W16_CALL "GDI", 9
.brush:
    cmp word [bp+8], 4              ; CTLCOLOR_DLG
    je .grey
    cmp word [bp+8], 6
    jne .no
.grey:
    push word 1                     ; LTGRAY_BRUSH
    W16_CALL "GDI", 87              ; GetStockObject
    jmp .ret
.command:
    push word [bp+14]               ; EndDialog(hwnd, wParam)
    push word [bp+10]
    W16_CALL "USER", 88
    mov ax, 1
.ret:
    pop ds
    pop bp
    retf 10

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc
W16_EXPORT 2, "DLGPROC", dlgproc

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
hwnd:       dw 0
hdc:        dw 0
hgreen:     dw 0
hyellow:    dw 0
oldbrush:   dw 0
numbuf:     times 16 db 0

wc:
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hcur:    dw 0
wc_hbr:     dw 0
            dd 0
            W16_FARPTR mainclass, 2

msg:
            dw 0, 0
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

ps:         times 32 db 0

dlgproc_ptr: W16_FARPTR dlgproc, 1

mainclass:  db "FullscrMain", 0
title:      db "Fullscreen test", 0
nosuch:     db "NOSUCH", 0
fmt_d:      db "%d", 0
ini_app:    db "Full", 0
ini_file:   db "FULLSCR.INI", 0
key_cursor: db "Cursor", 0
key_clickx: db "ClickX", 0
key_clicky: db "ClickY", 0
