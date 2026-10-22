; dialog.asm -- w16's test program for dialogs and the menu bar: a click,
; File > Number... or F2 (an accelerator) opens DIALOG 100 -- a number in
; an EDIT, OK and Cancel -- through DialogBox, whose procedure fills the
; field on WM_INITDIALOG and reads it back on OK; a right click or File >
; Ask... asks a Yes/No/Cancel MessageBox; File > Checked ticks and unticks
; itself (CheckMenuItem). The control menu gains "Always on Top" (GetSystemMenu,
; AppendMenu), which toggles SetWindowPos(HWND_TOPMOST / HWND_NOTOPMOST) and its
; tick, as Clock's does. Each answer is written to DIALOG.INI's [Dialog]
; section (Value=, Box=, Checked=, Top=), which the test reads back. At start it also holds a GetDC
; with NULL_PEN selected while it takes and releases a second one: NestedDC=1 when the two differ and
; the first still has its pen (Windows' DC cache; Paintbrush's brush strokes rely on it).
; The Number dialog's list is owner-drawn (LBS_OWNERDRAWFIXED): its rows are
; measured 20 tall (WM_MEASUREITEM) and filled dark grey, the selected one
; black (WM_DRAWITEM) -- the test reads the bands.
; Build: tools/gen_ne.py.
;
;w16: module DLGTEST
;w16: description Dialog test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: dialog 100 0 0 110 100 0x80c80080 "Number"
;w16: control 100 STATIC 65535 8 10 30 8 0x50000000 "&Value:"
;w16: control 100 EDIT 10 42 8 56 12 0x50810000 ""
;w16: control 100 BUTTON 1 8 34 44 14 0x50010001 "OK"
;w16: control 100 BUTTON 2 58 34 44 14 0x50010000 "Cancel"
;w16: control 100 LISTBOX 11 8 54 94 40 0x50800051 ""
;w16: menu 200
;w16: popup 200 "&File"
;w16: item 200 300 "&Number...\tF2"
;w16: item 200 301 "&Ask..."
;w16: separator 200
;w16: item 200 302 "&Checked" checked
;w16: item 200 303 "&Grayed" grayed
;w16: separator 200
;w16: item 200 304 "E&xit"
;w16: popup 200 "&Help"
;w16: item 200 305 "&About Dialog test..."
;w16: accel 210 0x71 300 virtkey

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F
%define WM_LBUTTONDOWN 0x0201
%define WM_RBUTTONDOWN 0x0204
%define WM_INITDIALOG  0x0110
%define WM_COMMAND     0x0111
%define WM_SYSCOMMAND  0x0112
%define WM_DRAWITEM    0x002B
%define WM_MEASUREITEM 0x002C
%define LB_ADDSTRING   0x0401
%define LB_SETCURSEL   0x0407
%define MF_CHECKED     0x0008
%define MF_SEPARATOR   0x0800
%define IDM_TOP        310
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
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

    push ds
    push word classname
    push ds
    push word title
    push word WS_OVERLAPPEDWINDOW >> 16
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
    push word 360
    push word 160
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow
    or ax, ax
    jz fail
    mov [hwnd], ax

    push word [hInstance]           ; SetMenu(hwnd, LoadMenu(hInst, MAKEINTRESOURCE(200)))
    push word 0
    push word 200
    W16_CALL "USER", 150
    mov [hmenu], ax
    push word [hwnd]
    push ax
    W16_CALL "USER", 158
    push word [hwnd]                ; GetSystemMenu(hwnd, FALSE)
    push word 0
    W16_CALL "USER", 156
    mov [hsysmenu], ax
    push ax                         ; AppendMenu(sys, MF_SEPARATOR, 0, NULL)
    push word MF_SEPARATOR
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 411
    push word [hsysmenu]            ; AppendMenu(sys, MF_STRING, IDM_TOP, "Always on &Top")
    push word 0
    push word IDM_TOP
    push ds
    push word top_text
    W16_CALL "USER", 411
    push word [hInstance]           ; LoadAccelerators(hInst, MAKEINTRESOURCE(210))
    push word 0
    push word 210
    W16_CALL "USER", 177
    mov [haccel], ax

    push word [hwnd]                ; GetDC(hwnd), NULL_PEN selected into it
    W16_CALL "USER", 66
    mov [hdc1], ax
    push word 8                     ; GetStockObject(NULL_PEN)
    W16_CALL "GDI", 87
    mov [null_pen], ax
    push word [hdc1]
    push ax
    W16_CALL "GDI", 45
    push word [hwnd]                ; GetDC(hwnd) while the first is held, then ReleaseDC
    W16_CALL "USER", 66
    mov [hdc2], ax
    push word [hwnd]
    push ax
    W16_CALL "USER", 68
    push word 7                     ; SelectObject(hdc1, BLACK_PEN): the old pen, still NULL_PEN?
    W16_CALL "GDI", 87
    push word [hdc1]
    push ax
    W16_CALL "GDI", 45
    xor cx, cx
    cmp ax, [null_pen]
    jne .nested
    mov dx, [hdc1]
    cmp dx, [hdc2]
    je .nested
    inc cx
.nested:
    mov [nested], cx
    push word [hwnd]                ; ReleaseDC(hwnd, hdc1)
    push word [hdc1]
    W16_CALL "USER", 68
    mov ax, [nested]
    mov di, ini_nested
    call save_ax

    push word [hwnd]
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
    push word [hwnd]                ; TranslateAccelerator(hwnd, haccel, &msg)
    push word [haccel]
    push ds
    push word msg
    W16_CALL "USER", 178
    or ax, ax
    jnz msgloop
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
    int 0x21

fail:
    mov ax, 0x4C01
    int 0x21

; [Dialog] <key>=AX, as a decimal: wsprintf, then WritePrivateProfileString.
; DI = the key's offset in DGROUP.
save_ax:
    push ax                         ; wsprintf(numbuf, "%d", ax) -- cdecl, the caller pops
    push ds
    push word fmt_d
    push ds
    push word numbuf
    W16_CALL "USER", 420
    add sp, 10
    push ds                         ; WritePrivateProfileString("Dialog", key, numbuf, "DIALOG.INI")
    push word ini_app
    push ds
    push di
    push ds
    push word numbuf
    push ds
    push word ini_file
    W16_CALL "KERNEL", 129
    ret

; LONG FAR PASCAL WndProc(HWND, UINT, WPARAM, LPARAM), as hello.asm's.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_PAINT
    je .paint
    cmp ax, WM_LBUTTONDOWN
    je .dialog
    cmp ax, WM_RBUTTONDOWN
    je .box
    cmp ax, WM_COMMAND
    je .command
    cmp ax, WM_DESTROY
    je .destroy
    cmp ax, WM_SYSCOMMAND
    jne .default
    cmp word [bp+10], IDM_TOP
    je .top
.default:
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
    push ax                         ; TextOut(hdc, 16, 16, line1)
    push word 16
    push word 16
    push ds
    push word line1
    push word line1_len
    W16_CALL "GDI", 33
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint(hwnd, &ps)
    jmp .zero
.dialog:
    push word [hInstance]           ; DialogBox(hInst, MAKEINTRESOURCE(100), hwnd, DlgProc)
    push word 0
    push word 100
    push word [bp+14]
    push word [dlgproc_ptr + 2]
    push word [dlgproc_ptr]
    W16_CALL "USER", 87
    cmp ax, 1
    jne .zero                       ; cancelled: nothing to keep
    mov ax, [value]
    mov di, ini_value
    call save_ax
    jmp .zero
.command:
    mov ax, [bp+10]
    cmp ax, 300
    je .dialog
    cmp ax, 301
    je .box
    cmp ax, 304
    je .exit
    cmp ax, 305
    je .about
    cmp ax, 302
    jne .zero
    xor word [checked], MF_CHECKED  ; CheckMenuItem(hmenu, 302, MF_BYCOMMAND | checked)
    push word [hmenu]
    push word 302
    push word [checked]
    W16_CALL "USER", 154
    mov ax, [checked]
    mov di, ini_checked
    call save_ax
    jmp .zero
.about:
    push word [bp+14]               ; ShellAbout(hwnd, "Dialog test", "w16's own sample", 0)
    push ds
    push word about_app
    push ds
    push word about_other
    push word 0
    W16_CALL "SHELL", 22
    jmp .zero
.exit:
    push word [bp+14]
    W16_CALL "USER", 53             ; DestroyWindow(hwnd)
    jmp .zero
.box:
    push word [bp+14]               ; MessageBox(hwnd, text, caption, MB_YESNOCANCEL | MB_ICONQUESTION)
    push ds
    push word box_text
    push ds
    push word title
    push word 0x0023
    W16_CALL "USER", 1
    mov di, ini_box
    call save_ax
    jmp .zero
.top:
    xor word [top], 1
    push word [bp+14]               ; SetWindowPos(hwnd, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
    mov ax, -2                      ;              SWP_NOMOVE | SWP_NOSIZE)
    add ax, [top]
    push ax
    push word 0
    push word 0
    push word 0
    push word 0
    push word 3
    W16_CALL "USER", 232
    mov ax, [top]                   ; CheckMenuItem(sys, IDM_TOP, top ? MF_CHECKED : MF_UNCHECKED)
    mov cl, 3
    shl ax, cl
    push word [hsysmenu]
    push word IDM_TOP
    push ax
    W16_CALL "USER", 154
    mov ax, [top]
    mov di, ini_top
    call save_ax
    jmp .zero
.destroy:
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage(0)
.zero:
    xor ax, ax
    xor dx, dx
.ret:
    pop ds
    pop bp
    retf 10

; BOOL FAR PASCAL DlgProc(HWND, UINT, WPARAM, LPARAM): the field starts at
; 42; OK keeps what it holds, Cancel nothing.
dlgproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_INITDIALOG
    je .init
    cmp ax, WM_COMMAND
    je .command
    cmp ax, WM_MEASUREITEM
    je .measure
    cmp ax, WM_DRAWITEM
    je .draw
.no:
    xor ax, ax
    jmp .ret
.measure:
    les bx, [bp+6]                  ; MEASUREITEMSTRUCT: itemHeight 20
    mov word [es:bx+8], 20
    mov ax, 1
    jmp .ret
.draw:
    les bx, [bp+6]                  ; DRAWITEMSTRUCT: a stock brush by itemState
    mov ax, 3                       ; DKGRAY_BRUSH
    test word [es:bx+8], 1          ; ODS_SELECTED
    jz .brush
    mov ax, 4                       ; BLACK_BRUSH
.brush:
    push es
    push bx
    push ax
    W16_CALL "GDI", 87              ; GetStockObject
    pop bx
    pop es
    push word [es:bx+12]            ; FillRect(hDC, &rcItem, brush)
    push es
    lea cx, [bx+14]
    push cx
    push ax
    W16_CALL "USER", 81
    mov ax, 1
    jmp .ret
.init:
    mov cx, 3                       ; three rows, the middle one selected
.row:
    push cx
    push word [bp+14]               ; SendDlgItemMessage(hDlg, 11, LB_ADDSTRING, 0, "Row")
    push word 11
    push word LB_ADDSTRING
    push word 0
    push ds
    push word row_text
    W16_CALL "USER", 101
    pop cx
    loop .row
    push word [bp+14]               ; SendDlgItemMessage(hDlg, 11, LB_SETCURSEL, 1, 0)
    push word 11
    push word LB_SETCURSEL
    push word 1
    push word 0
    push word 0
    W16_CALL "USER", 101
    push word [bp+14]               ; SetDlgItemInt(hDlg, 10, 42, FALSE)
    push word 10
    push word 42
    push word 0
    W16_CALL "USER", 94
    mov ax, 1
    jmp .ret
.command:
    mov ax, [bp+10]
    cmp ax, 1
    je .ok
    cmp ax, 2
    jne .no
    push word [bp+14]               ; EndDialog(hDlg, 0)
    push word 0
    W16_CALL "USER", 88
    mov ax, 1
    jmp .ret
.ok:
    push word [bp+14]               ; GetDlgItemInt(hDlg, 10, NULL, FALSE)
    push word 10
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 95
    mov [value], ax
    push word [bp+14]               ; EndDialog(hDlg, 1)
    push word 1
    W16_CALL "USER", 88
    mov ax, 1
.ret:
    xor dx, dx
    pop ds
    pop bp
    retf 10

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc
W16_EXPORT 2, "DLGPROC", dlgproc

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0                   ; the task's reserved words

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
value:      dw 0
hmenu:      dw 0
haccel:     dw 0
checked:    dw MF_CHECKED
hsysmenu:   dw 0
top:        dw 0
dlgproc_ptr: W16_FARPTR dlgproc, 1

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
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

ps:         times 32 db 0
numbuf:     times 16 db 0
hdc1:       dw 0
hdc2:       dw 0
null_pen:   dw 0
nested:     dw 0

classname:  db "DlgTestClass", 0
title:      db "Dialog test (Win16)", 0
line1:      db "Click: a dialog. Right-click: a message box."
line1_len   equ $ - line1
box_text:   db "Save the changes?", 0
fmt_d:      db "%d", 0
ini_app:    db "Dialog", 0
ini_value:  db "Value", 0
ini_box:    db "Box", 0
ini_file:   db "DIALOG.INI", 0
ini_checked: db "Checked", 0
ini_top:    db "Top", 0
ini_nested: db "NestedDC", 0
top_text:   db "Always on &Top", 0
row_text:   db "Row", 0
about_app:  db "Dialog test", 0
about_other: db "w16's own sample", 0
