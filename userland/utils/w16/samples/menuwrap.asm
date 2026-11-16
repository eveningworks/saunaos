; menuwrap.asm -- a menu bar wider than its window, which 3.1 wrapped onto more rows, as WinBench 3.1's
; did. tools/tests/gui/w16_gui_test.py's `menuwrap` checks each by MENUWRAP.INI or by pixel:
;
;   - a window 210 wide ("Menu wrap test") whose bar is Alpha, Bravo, Charlie, Delta and "\b&Help":
;     Delta does not fit after Charlie, so it starts a second row, and Help -- its text starting "\b",
;     WinBench's own -- ends at that row's right edge
;   - every WM_SIZE writes the client height (Height=, three digits): the client is under both rows
;   - Delta > Shorten (104) deletes Charlie and Delta (DeleteMenu by position) and calls DrawMenuBar:
;     what is left fits on one row, so the client grows by a row and WM_SIZE says so
;   - a command writes Chosen=<id - 100>
;
; Build: tools/gen/gen_ne.py.
;
;w16: module MENUWRAP
;w16: description Menu wrap test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: menu 1
;w16: popup 1 "&Alpha"
;w16: item 1 101 "&One"
;w16: popup 1 "&Bravo"
;w16: item 1 102 "&Two"
;w16: popup 1 "&Charlie"
;w16: item 1 103 "&Three"
;w16: popup 1 "&Delta"
;w16: item 1 104 "&Shorten"
;w16: popup 1 "\b&Help"
;w16: item 1 105 "&About"

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_SIZE        0x0005
%define WM_COMMAND     0x0111
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW   5
%define IDC_ARROW      32512
%define MF_BYPOSITION  0x0400

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
    W16_CALL "USER", 57             ; RegisterClass: menu 1
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
    push word 210
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
    push ax
    push word [nCmdShow]
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
    mov ax, 0x4C00
    int 0x21

fail:
    mov ax, 0x4C01
    int 0x21

; WritePrivateProfileString("MenuWrap", SI, DI, "MENUWRAP.INI")
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

; DeleteMenu(GetMenu(hwnd), 2, MF_BYPOSITION)
delete_third:
    push word [hwnd]
    W16_CALL "USER", 157            ; GetMenu
    push ax
    push word 2
    push word MF_BYPOSITION
    W16_CALL "USER", 413            ; DeleteMenu
    ret

; LONG FAR PASCAL WndProc: [bp+14] hwnd, [bp+12] msg, [bp+10] wParam, [bp+8]:[bp+6] lParam.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    push si
    push di
    mov ax, [bp+12]
    cmp ax, WM_SIZE
    je .size
    cmp ax, WM_COMMAND
    je .command
    cmp ax, WM_DESTROY
    je .destroy
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret

.size:
    mov ax, [bp+8]                  ; HIWORD(lParam): the client height, as three digits
    mov bl, 100
    div bl                          ; AL hundreds, AH the rest
    add al, '0'
    mov [num3], al
    mov al, ah
    xor ah, ah
    mov bl, 10
    div bl
    add al, '0'
    mov [num3 + 1], al
    add ah, '0'
    mov [num3 + 2], ah
    mov si, key_height
    mov di, num3
    call ini_write
    jmp .zero

.command:
    mov ax, [bp+10]
    cmp ax, 104
    jne .say
    call delete_third               ; Charlie, then Delta in its place
    call delete_third
    push word [hwnd]
    W16_CALL "USER", 160            ; DrawMenuBar: one row now
    mov ax, [bp+10]
.say:
    sub ax, 100
    add al, '0'
    mov [num1], al
    mov si, key_chosen
    mov di, num1
    call ini_write
    jmp .zero

.destroy:
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
.zero:
    xor ax, ax
    xor dx, dx
.ret:
    pop di
    pop si
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

wc:
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
            dw COLOR_WINDOW + 1
            dw 1, 0                 ; MAKEINTRESOURCE(1): the class's menu
            W16_FARPTR classname, 2

msg:
            dw 0, 0
            dw 0
            dd 0, 0
            dw 0, 0

classname:  db "MenuWrapClass", 0
title:      db "Menu wrap test", 0
ini_app:    db "MenuWrap", 0
ini_file:   db "MENUWRAP.INI", 0
key_height: db "Height", 0
key_chosen: db "Chosen", 0
num3:       db "000", 0
num1:       db "0", 0
