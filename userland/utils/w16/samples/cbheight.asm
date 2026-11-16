; cbheight.asm -- a combo box's shut height and frame, per font, as 3.1 made them: the Windows
; Tutorial's sundae dialog (Windows Basics, page 39) has a WS_BORDER drop-down list in the System font.
; tools/tests/gui/w16_gui_test.py's `combo_height` checks. For each of eight dialogs -- the System font,
; Helv 8, 10, 12, 14, 18 and 24, Small Fonts 6 -- three combo boxes:
;
;   id 10 a drop-down list with WS_BORDER, id 11 one without, id 12 a drop-down (an edit) with WS_BORDER
;
; and writes, in hex, to CBHEIGHT.INI [CbHeight]: Dn=tmHeight of the dialog's font, then
; DnHid=the box's height (GetWindowRect) and DnBid=1 when its style still has WS_BORDER. Done=1 last.
;
; Build: tools/gen/gen_ne.py.
;
;w16: module CBHEIGHT
;w16: description Combo height test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: dialog 100 10 10 200 90 0x90c80080 "System"
;w16: control 100 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 100 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 100 COMBOBOX 12 8 40 80 60 0x50a00002 ""
;w16: dialog 101 10 10 200 90 0x90c80080 "Helv 8" font 8 "Helv"
;w16: control 101 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 101 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 101 COMBOBOX 12 8 40 80 60 0x50a00002 ""
;w16: dialog 102 10 10 200 90 0x90c80080 "Helv 10" font 10 "Helv"
;w16: control 102 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 102 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 102 COMBOBOX 12 8 40 80 60 0x50a00002 ""
;w16: dialog 103 10 10 200 90 0x90c80080 "Helv 12" font 12 "Helv"
;w16: control 103 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 103 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 103 COMBOBOX 12 8 40 80 60 0x50a00002 ""
;w16: dialog 104 10 10 200 90 0x90c80080 "Helv 14" font 14 "Helv"
;w16: control 104 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 104 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 104 COMBOBOX 12 8 40 80 60 0x50a00002 ""
;w16: dialog 105 10 10 200 90 0x90c80080 "Helv 18" font 18 "Helv"
;w16: control 105 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 105 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 105 COMBOBOX 12 8 40 80 60 0x50a00002 ""
;w16: dialog 106 10 10 200 90 0x90c80080 "Helv 24" font 24 "Helv"
;w16: control 106 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 106 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 106 COMBOBOX 12 8 40 80 60 0x50a00002 ""
;w16: dialog 107 10 10 200 90 0x90c80080 "Small Fonts 6" font 6 "Small Fonts"
;w16: control 107 COMBOBOX 10 8 8 80 60 0x50a00003 ""
;w16: control 107 COMBOBOX 11 100 8 80 60 0x50200003 ""
;w16: control 107 COMBOBOX 12 8 40 80 60 0x50a00002 ""

%include "ne16.inc"

%define WM_DESTROY  0x0002
%define WM_GETFONT  0x0031
%define WM_INITDIALOG 0x0110
%define CW_USEDEFAULT 0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
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

    mov word [dlgid], 100
    mov byte [key_d + 1], '0'
.dialog:
    push word [hInstance]           ; CreateDialog(hInst, MAKEINTRESOURCE(dlgid), hwnd, DlgProc)
    push word 0
    push word [dlgid]
    push word [hwnd]
    push word [dlgproc_ptr + 2]
    push word [dlgproc_ptr]
    W16_CALL "USER", 89
    or ax, ax
    jz fail
    mov [hdlg], ax

    push ax                         ; the dialog's font: WM_GETFONT, 0 for the System font
    push word WM_GETFONT
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 111            ; SendMessage
    mov [hfont], ax
    push word [hdlg]
    W16_CALL "USER", 66             ; GetDC
    mov [hdc], ax
    cmp word [hfont], 0
    je .sysfont
    push ax
    push word [hfont]
    W16_CALL "GDI", 45              ; SelectObject
.sysfont:
    push word [hdc]
    push ds
    push word tm
    W16_CALL "GDI", 93              ; GetTextMetrics
    push word [hdlg]
    push word [hdc]
    W16_CALL "USER", 68             ; ReleaseDC
    mov al, [tm]                    ; tmHeight
    mov di, num_t
    call hex2
    mov byte [key_d + 2], 0         ; "Dn"
    mov si, key_d
    mov di, num_t
    call ini_write

    mov word [ctlid], 10
.control:
    push word [hdlg]
    push word [ctlid]
    W16_CALL "USER", 91             ; GetDlgItem
    mov [hctl], ax
    push ax
    push ds
    push word rc
    W16_CALL "USER", 32             ; GetWindowRect
    mov ax, [rc + 6]
    sub ax, [rc + 2]
    mov di, num_t
    call hex2
    mov byte [key_d + 2], 'H'
    mov al, [ctlid]
    add al, '0' - 10 + 0            ; 10 -> '0', 11 -> '1', 12 -> '2'
    mov [key_d + 3], al
    mov byte [key_d + 4], 0
    mov si, key_d
    mov di, num_t
    call ini_write
    push word [hctl]
    push word GWL_STYLE
    W16_CALL "USER", 135            ; GetWindowLong
    mov di, zero_t
    test dx, 0x0080                 ; WS_BORDER's bit, high word
    jz .noborder
    mov di, one_t
.noborder:
    mov byte [key_d + 2], 'B'
    mov si, key_d
    call ini_write
    inc word [ctlid]
    cmp word [ctlid], 13
    jb .control

    push word [hdlg]
    W16_CALL "USER", 53             ; DestroyWindow
    inc word [dlgid]
    inc byte [key_d + 1]
    cmp word [dlgid], 108
    jb .dialog

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

; WritePrivateProfileString("CbHeight", SI, DI, "CBHEIGHT.INI")
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
hdc:        dw 0
hfont:      dw 0
dlgid:      dw 0
ctlid:      dw 0
rc:         dw 0, 0, 0, 0
tm:         times 32 db 0

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
classname:  db "CbHeightClass", 0
title:      db "Combo height test", 0
ini_app:    db "CbHeight", 0
ini_file:   db "CBHEIGHT.INI", 0
key_d:      db "D0??", 0, 0
key_done:   db "Done", 0
num_t:      db "00", 0
one_t:      db "1", 0
zero_t:     db "0", 0
