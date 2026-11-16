; lbrect.asm -- where a control's border lies against its template rectangle, as 3.1 placed it: the
; Windows Tutorial's Open dialog (Windows Basics, pages 33-37) has a WS_BORDER list box whose frame
; 3.11 draws a pixel OUTSIDE the template's rectangle, where its edit box's frame is inside.
; tools/tests/gui/w16_gui_test.py's `border_rects` checks. Dialog 100 (Helv 8) holds, all at template
; (8, 8, 60, 40) in turn down the dialog, with LBS_NOINTEGRALHEIGHT so no height is trimmed:
;
;   id 10 a LISTBOX with WS_BORDER, 11 one without, 12 an EDIT with WS_BORDER, 13 a STATIC with WS_BORDER
;   id 14 a LISTBOX with WS_BORDER and WS_VSCROLL
;
; and the main window a LISTBOX with WS_BORDER made by CreateWindow at (10, 10, 100, 80) (id 20).
; LBRECT.INI [LbRect] gets, in hex, Xid/Yid (the window's corner in its parent's client area), Wid/Hid,
; and Bid=1 when GetWindowLong still has WS_BORDER. Then dialog 101 ("Second"), empty, beside the
; first: the test clicks between the three windows and reads which caption is lit. Done=1 last.
;
; Build: tools/gen/gen_ne.py.
;
;w16: module LBRECT
;w16: description Border rectangle test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: dialog 100 10 10 220 200 0x90c80080 "Helv 8" font 8 "Helv"
;w16: control 100 LISTBOX 10 8 8 60 40 0x50810100 ""
;w16: control 100 LISTBOX 11 80 8 60 40 0x50010100 ""
;w16: control 100 EDIT 12 150 8 60 40 0x50810080 ""
;w16: control 100 STATIC 13 8 60 60 40 0x50800000 ""
;w16: control 100 LISTBOX 14 80 60 60 40 0x50a10100 ""
;w16: dialog 101 230 10 60 40 0x90c80080 "Second"

%include "ne16.inc"

%define WM_DESTROY  0x0002
%define WM_INITDIALOG 0x0110
%define CW_USEDEFAULT 0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define LB_STYLE    0x50800100      ; WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOINTEGRALHEIGHT
%define COLOR_WINDOW 5
%define IDC_ARROW   32512
%define GWL_STYLE   -16

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
    push word 640
    push word 420
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

    push ds                         ; CreateWindow("LISTBOX", ..., 10, 10, 100, 80, hwnd, 20)
    push word lbclass
    push ds
    push word empty
    push word LB_STYLE >> 16
    push word LB_STYLE & 0xFFFF
    push word 10
    push word 10
    push word 100
    push word 80
    push word [hwnd]
    push word 20
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41
    or ax, ax
    jz fail

    push word [hInstance]           ; CreateDialog(hInst, MAKEINTRESOURCE(100), hwnd, DlgProc)
    push word 0
    push word 100
    push word [hwnd]
    push word [dlgproc_ptr + 2]
    push word [dlgproc_ptr]
    W16_CALL "USER", 89
    or ax, ax
    jz fail
    mov [hdlg], ax

    mov word [rid], 10
.ctl:
    mov ax, [hdlg]
    mov [rparent], ax
    call report
    inc word [rid]
    cmp word [rid], 15
    jb .ctl
    mov ax, [hwnd]
    mov [rparent], ax
    mov word [rid], 20
    call report

    push word [hInstance]           ; CreateDialog(hInst, MAKEINTRESOURCE(101), hwnd, DlgProc): a second,
    push word 0                     ; empty one beside the first, which it dims
    push word 101
    push word [hwnd]
    push word [dlgproc_ptr + 2]
    push word [dlgproc_ptr]
    W16_CALL "USER", 89

    mov si, key_done
    mov di, one_t
    call ini_write

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

; Control [rid] of [rparent]: its corner in the parent's client area, its size and WS_BORDER, to the INI.
report:
    push word [rparent]
    push word [rid]
    W16_CALL "USER", 91             ; GetDlgItem
    mov [hctl], ax
    push ax
    push ds
    push word rc
    W16_CALL "USER", 32             ; GetWindowRect
    mov ax, [rc]
    mov [pt], ax
    mov ax, [rc + 2]
    mov [pt + 2], ax
    push word [rparent]
    push ds
    push word pt
    W16_CALL "USER", 29             ; ScreenToClient
    mov al, 'X'
    mov bx, [pt]
    call put
    mov al, 'Y'
    mov bx, [pt + 2]
    call put
    mov al, 'W'
    mov bx, [rc + 4]
    sub bx, [rc]
    call put
    mov al, 'H'
    mov bx, [rc + 6]
    sub bx, [rc + 2]
    call put
    push word [hctl]
    push word GWL_STYLE
    W16_CALL "USER", 135            ; GetWindowLong
    xor bx, bx
    test dx, 0x0080                 ; WS_BORDER's bit, high word
    jz .nb
    inc bx
.nb:
    mov al, 'B'
    call put
    ret

; Key AL followed by [rid] in decimal, value BX as four hex digits, to the INI.
put:
    mov [key_t], al
    mov ax, [rid]
    mov cl, 10
    div cl
    add ax, '00'
    mov [key_t + 1], ax
    mov di, num_t
    mov al, bh
    call hex2
    mov al, bl
    call hex2
    mov si, key_t
    mov di, num_t
    call ini_write
    ret

; AL as two hex digits at DS:DI, DI moved on
hex2:
    mov ah, al
    shr al, 4
    call .d
    mov al, ah
    and al, 0xF
.d:
    add al, '0'
    cmp al, '9'
    jbe .s
    add al, 7
.s:
    mov [di], al
    inc di
    ret

; WritePrivateProfileString("LbRect", SI, DI, "LBRECT.INI")
ini_write:
    push ds
    push word ini_app
    push ds
    push si
    push ds
    push di
    push ds
    push word ini_file
    W16_CALL "KERNEL", 129
    ret

; BOOL FAR PASCAL DlgProc: TRUE to WM_INITDIALOG, FALSE to the rest.
dlgproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    xor ax, ax
    cmp word [bp+12], WM_INITDIALOG
    jne .ret
    inc ax
.ret:
    xor dx, dx
    pop ds
    pop bp
    retf 10

; LONG FAR PASCAL WndProc
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
W16_EXPORT 2, "DLGPROC", dlgproc

W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
hdlg:       dw 0
hctl:       dw 0
rparent:    dw 0
rid:        dw 0
rc:         dw 0, 0, 0, 0
pt:         dw 0, 0

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

dlgproc_ptr: W16_FARPTR dlgproc, 1
classname:  db "LbRectClass", 0
lbclass:    db "LISTBOX", 0
empty:      db 0
title:      db "Border rectangle test", 0
ini_app:    db "LbRect", 0
ini_file:   db "LBRECT.INI", 0
key_t:      db "X00", 0
key_done:   db "Done", 0
num_t:      db "0000", 0
one_t:      db "1", 0
