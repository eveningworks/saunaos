; dlgplace.asm -- where 3.1 puts a byte-aligned window, measured against 3.11: a WS_POPUP main window at
; 0, 0 owns three modeless dialogs and four popups of its own classes, each at a chosen x -- seven, as
; many as a w16 window may have beside it (WIN_CLIENT_MAX).
;   101      DIALOG, 8-point Helv (DS_SETFONT), x 146 dialog units: the window asked at about 250
;   105 106  DIALOG, the System font, x 20 and 21: asked at 35 and 37
;   W35 W36  a class with CS_BYTEALIGNWINDOW, the window asked at that x
;   C402 C403  a class with CS_BYTEALIGNCLIENT and a 1-pixel border: the client asked at x + 1
; Each dialog is titled with its id and its class's style word. On 3.11 (2026-11-16, with more of each)
; the dialog class is 2808 (CS_BYTEALIGNWINDOW | CS_SAVEBITS | CS_DBLCLKS) and the windows are at 248,
; 32 40, 32 40, 399 407: the aligned edge on the NEAREST multiple of 8, a remainder of 4 going up.
; tools/tests/gui/w16_gui_test.py checks every window's x against 3.11's.
; Build: tools/gen/gen_ne.py.
;
;w16: module DLGPLACE
;w16: description Byte-alignment test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: dialog 101 146 0 60 24 0x90c80080 "A146" font 8 "Helv"
;w16: dialog 105 20 0 50 24 0x90c80080 "B20"
;w16: dialog 106 21 45 50 24 0x90c80080 "B21"

%include "ne16.inc"

%define WM_DESTROY      0x0002
%define WM_INITDIALOG   0x0110
%define WS_POPUP_VIS    0x9000          ; WS_POPUP | WS_VISIBLE, the high word
%define WS_POPUP_CAP    0x90C0          ; ... | WS_CAPTION
%define COLOR_APPWORKSPACE 12
%define IDC_ARROW       32512
%define CS_BYTEALIGNCLIENT 0x1000
%define CS_BYTEALIGNWINDOW 0x2000

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
    mov word [wc_style], CS_BYTEALIGNWINDOW
    mov word [wc_name], wclass
    push ds
    push word wc
    W16_CALL "USER", 57
    mov word [wc_style], CS_BYTEALIGNCLIENT
    mov word [wc_name], cclass
    push ds
    push word wc
    W16_CALL "USER", 57

    push ds
    push word mainclass
    push ds
    push word title
    push word WS_POPUP_VIS
    push word 0
    push word 0                     ; at 0, 0, 600 x 340
    push word 0
    push word 600
    push word 340
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: the main window
    or ax, ax
    jz fail
    mov [hwnd], ax

    mov bx, dialogs
.dialog:
    mov si, [bx]
    push bx
    push word [hInstance]           ; CreateDialog(hInst, MAKEINTRESOURCE(si), hwnd, DlgProc)
    push word 0
    push si
    push word [hwnd]
    push word [dlgproc_ptr + 2]
    push word [dlgproc_ptr]
    W16_CALL "USER", 89
    or ax, ax
    jz .next
    mov di, ax
    push ax                         ; GetClassWord(hdlg, GCW_STYLE)
    push word -26
    W16_CALL "USER", 129
    push ax                         ; wsprintf(buf, "%d %04X", si, style) -- cdecl, the caller pops
    push si
    push ds
    push word fmt
    push ds
    push word buf
    W16_CALL "USER", 420
    add sp, 12
    push di                         ; SetWindowText(hdlg, buf)
    push ds
    push word buf
    W16_CALL "USER", 37
.next:
    pop bx
    add bx, 2
    cmp word [bx], 0
    jne .dialog

    mov bx, popups
.popup:
    push bx
    push ds                         ; CreateWindow(class, title, WS_POPUP|WS_VISIBLE|WS_CAPTION, x, y, 90, 28, hwnd)
    push word [bx]
    push ds
    push word [bx + 2]
    push word WS_POPUP_CAP
    push word 0
    push word [bx + 4]
    push word [bx + 6]
    push word 90
    push word 28
    push word [hwnd]
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41
    pop bx
    add bx, 8
    cmp word [bx], 0
    jne .popup

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
    mov ax, 0x4C00
    int 0x21
fail:
    mov ax, 0x4C01
    int 0x21

; Every class's procedure, (hwnd, msg, wParam, lParam), Pascal. DS is SS, the program's own data, so it
; runs on 3.11 with no MakeProcInstance.
wndproc:
    push bp
    mov bp, sp
    push ds
    push ss
    pop ds
    mov ax, [bp+14]
    cmp ax, [hwnd]
    jne .default
    cmp word [bp+12], WM_DESTROY
    jne .default
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
    xor ax, ax
    xor dx, dx
    jmp .ret
.default:
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
.ret:
    pop ds
    pop bp
    retf 10

; BOOL FAR PASCAL DlgProc(HWND, UINT, WPARAM, LPARAM): TRUE for WM_INITDIALOG, else FALSE.
dlgproc:
    xor ax, ax
    push bp
    mov bp, sp
    cmp word [bp+12], WM_INITDIALOG
    jne .ret
    inc ax
.ret:
    xor dx, dx
    pop bp
    retf 10

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc
W16_EXPORT 2, "DLGPROC", dlgproc

W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
hwnd:       dw 0
dlgproc_ptr: W16_FARPTR dlgproc, 1

wc:
wc_style:   dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
            dw COLOR_APPWORKSPACE + 1
            dd 0
wc_name:    W16_FARPTR mainclass, 2

; class, title, x, y -- a 0 class ends the list
dialogs:    dw 101, 105, 106, 0
popups:     dw wclass, tw35, 35, 390
            dw wclass, tw36, 36, 420
            dw cclass, tc402, 402, 390
            dw cclass, tc403, 403, 420
            dw 0

msg:        times 18 db 0
fmt:        db "%d %04X", 0
buf:        times 32 db 0

mainclass:  db "PlaceMain", 0
wclass:     db "AlignWindow", 0
cclass:     db "AlignClient", 0
title:      db "Byte alignment", 0
tw35:       db "W35", 0
tw36:       db "W36", 0
tc402:      db "C402", 0
tc403:      db "C403", 0
