; dlghide.asm -- a modeless dialog shown, read off the screen and hidden, as the Windows Tutorial's
; Open dialog is: it photographs the dialog with a screen DC and hides it, then paints the photograph
; and its tips on its own window. tools/tests/gui/w16_gui_test.py's `dialog_hide` checks:
;
;   - a main window ("Dialog hide test"), then CreateDialog of template 100 ("Hide me", WS_VISIBLE):
;     GetPixel on the screen DC in its caption straight after (Caption0) and once its messages are
;     taken (Caption), as the Tutorial photographs its Open dialog -- 3.11's answers in DLGHIDE.INI
;   - OkStyleFocus0 / OkStyleFocus: OK's button style (low nibble) and the focus's id, straight after
;     and once its messages are taken: the template's plain OK stays a plain push button while the
;     edit (104, 0x68) has the first focus -- 3.11 answers 0068 both times
;   - AfterSetFocus: the caption once SetFocus has gone to the main window, its owner -- the dialog
;     made inactive inside the call (3.11: FFFFFF); AfterSetActive the same after SetActiveWindow
;   - ShowWindow(dialog, SW_HIDE): the dialog's window goes from the screen (Hidden=1 once done)
;
; Build: tools/gen/gen_ne.py.
;
;w16: module DLGHIDE
;w16: description Dialog hide test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: dialog 100 20 20 120 40 0x90c80080 "Hide me"
;w16: control 100 EDIT 104 8 4 60 12 0x50810080 ""
;w16: control 100 BUTTON 1 72 4 40 14 0x50010000 "OK"
;w16: control 100 BUTTON 2 72 20 40 14 0x50010000 "Cancel"

%include "ne16.inc"

%define WM_DESTROY  0x0002
%define WM_INITDIALOG 0x0110
%define CW_USEDEFAULT 0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW 5
%define IDC_ARROW   32512

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
    push word 400
    push word 300
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

    mov si, key_cap0                ; straight after CreateDialog
    call read_caption
    mov si, key_st0
    call read_okstyle
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
    mov si, key_cap                 ; once its messages are taken, as the Tutorial's photograph is
    call read_caption
    mov si, key_st1
    call read_okstyle
    push word [hwnd]
    W16_CALL "USER", 22             ; SetFocus(main window, the dialog's owner)
    mov si, key_cap2
    call read_caption
    push word [hwnd]
    W16_CALL "USER", 59             ; SetActiveWindow(main window)
    mov si, key_cap3
    call read_caption

    push word [hdlg]
    push word 0
    W16_CALL "USER", 42             ; ShowWindow(hdlg, SW_HIDE)
    mov si, key_hidden
    mov di, one_t
    call ini_write
    jmp msgloop

; The screen's pixel in the dialog's caption, 40 in and 10 down, to the INI under key SI.
read_caption:
    push si
    push word [hdlg]                ; GetWindowRect(hdlg, &rc)
    push ds
    push word rc
    W16_CALL "USER", 32
    push word 0
    W16_CALL "USER", 66             ; GetDC(NULL): the screen
    mov [hdc], ax
    push ax                         ; GetPixel(hdc, rc.left + 40, rc.top + 10): in its caption
    mov ax, [rc]
    add ax, 40
    push ax
    mov ax, [rc + 2]
    add ax, 10
    push ax
    W16_CALL "GDI", 83             ; GetPixel
    mov [pix], ax
    mov [pix + 2], dx
    push word 0
    push word [hdc]
    W16_CALL "USER", 68             ; ReleaseDC
    mov al, [pix]                   ; COLORREF 00BBGGRR as RRGGBB
    mov di, cap_t
    call hex2
    mov al, [pix + 1]
    call hex2
    mov al, [pix + 2]
    call hex2
    pop si
    mov di, cap_t
    call ini_write
    ret

; OK's button style (GetWindowLong & 0xF: 0 a push button, 1 the default) and the focus's id, to the INI under SI.
read_okstyle:
    push si
    push word [hdlg]
    push word 1
    W16_CALL "USER", 91             ; GetDlgItem(hdlg, IDOK)
    push ax
    push word -16
    W16_CALL "USER", 135            ; GetWindowLong(GWL_STYLE)
    and al, 0x0F
    mov di, cap_t
    call hex2
    W16_CALL "USER", 23             ; GetFocus
    push ax
    W16_CALL "USER", 277            ; GetDlgCtrlID
    call hex2
    mov byte [di], 0
    pop si
    mov di, cap_t
    call ini_write
    ret

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

; WritePrivateProfileString("DlgHide", SI, DI, "DLGHIDE.INI")
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
hdc:        dw 0
pix:        dd 0
rc:         dw 0, 0, 0, 0

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
classname:  db "DlgHideClass", 0
title:      db "Dialog hide test", 0
ini_app:    db "DlgHide", 0
ini_file:   db "DLGHIDE.INI", 0
key_cap:    db "Caption", 0
key_cap0:   db "Caption0", 0
key_cap2:   db "AfterSetFocus", 0
key_st0:    db "OkStyleFocus0", 0
key_st1:    db "OkStyleFocus", 0
key_cap3:   db "AfterSetActive", 0
key_hidden: db "Hidden", 0
cap_t:      db "000000", 0
one_t:      db "1", 0
