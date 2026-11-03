; mdi.asm -- w16's test program for the Multiple Document Interface: a frame ("MDI test") with a
; Window menu, an MDICLIENT, and two document windows made by WM_MDICREATE, "One" and "Two" -- Two,
; made last, is the active one. Window > Tile and Cascade send WM_MDITILE and WM_MDICASCADE;
; Maximize and Restore maximize or restore the active document (WM_MDIGETACTIVE, then
; WM_MDIMAXIMIZE / WM_MDIRESTORE); the message loop runs TranslateMDISysAccel, so Ctrl+F6 goes to
; the next document. tools/tests/gui/w16_gui_test.py checks the captions lit and dimmed, Ctrl+F6, and
; the frame's title and bar while a document is maximized.
; Build: tools/gen/gen_ne.py.
;
;w16: module MDI
;w16: description MDI test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_COMMAND     0x0111
%define WM_MDICREATE   0x0220
%define WM_MDIRESTORE  0x0223
%define WM_MDIMAXIMIZE 0x0225
%define WM_MDITILE     0x0226
%define WM_MDICASCADE  0x0227
%define WM_MDIGETACTIVE 0x0229
%define CW_USEDEFAULT  0x8000
%define MF_POPUP       0x0010
%define COLOR_WINDOW   5
%define COLOR_APPWORKSPACE 12
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
    W16_CALL "USER", 5              ; InitApp

    mov ax, [hInstance]
    mov [wc_hInstance], ax
    mov [mcs1_owner], ax
    mov [mcs2_owner], ax
    push word 0
    push word 0
    push word IDC_ARROW
    W16_CALL "USER", 173            ; LoadCursor
    mov [wc_hCursor], ax
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass: the frame's
    or ax, ax
    jz fail
    mov word [wc_proc], docproc     ; the document's: its own procedure and the window's colour
    mov word [wc_hbr], COLOR_WINDOW + 1
    mov word [wc_name], docclass
    push ds
    push word wc
    W16_CALL "USER", 57
    or ax, ax
    jz fail

    W16_CALL "USER", 151            ; CreateMenu: the bar
    mov [hbar], ax
    W16_CALL "USER", 415            ; CreatePopupMenu: Window
    mov [hwinmenu], ax
    mov [ccs_menu], ax
    xor si, si                      ; SI: a call keeps it, as Pascal callees did
.items:
    push word [hwinmenu]            ; AppendMenu(hwinmenu, MF_STRING, id, text)
    push word 0
    mov ax, si
    shr ax, 1
    inc ax
    push ax
    push ds
    push word [item_text + si]
    W16_CALL "USER", 411
    add si, 2
    cmp si, 8
    jb .items
    push word [hbar]                ; AppendMenu(hbar, MF_POPUP, hwinmenu, "&Window")
    push word MF_POPUP
    push word [hwinmenu]
    push ds
    push word winmenu_text
    W16_CALL "USER", 411

    push ds
    push word frameclass
    push ds
    push word title
    push word 0x02CF                ; WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN
    push word 0
    push word CW_USEDEFAULT
    push word CW_USEDEFAULT
    push word 400
    push word 260
    push word 0
    push word [hbar]
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: the frame
    or ax, ax
    jz fail
    mov [hframe], ax

    push ds
    push word clientclass
    push word 0
    push word 0
    push word 0x5200                ; WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE
    push word 0
    push word 0
    push word 0
    push word 0
    push word 0
    push word [hframe]
    push word 1
    push word [hInstance]
    push ds
    push word ccs
    W16_CALL "USER", 41             ; CreateWindow: the MDICLIENT, its CLIENTCREATESTRUCT
    or ax, ax
    jz fail
    mov [hclient], ax

    push word [hframe]
    push word [nCmdShow]
    W16_CALL "USER", 42             ; ShowWindow: its WM_SIZE sizes the client (DefFrameProc)
    push word [hframe]
    W16_CALL "USER", 124            ; UpdateWindow

    push word [hclient]
    push word WM_MDICREATE
    push word 0
    push ds
    push word mcs1
    W16_CALL "USER", 111            ; SendMessage: document One
    push word [hclient]
    push word WM_MDICREATE
    push word 0
    push ds
    push word mcs2
    W16_CALL "USER", 111            ; ...and Two, the active one

msgloop:
    push ds
    push word msg
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 108            ; GetMessage
    or ax, ax
    jz quit
    push word [hclient]
    push ds
    push word msg
    W16_CALL "USER", 451            ; TranslateMDISysAccel: Ctrl+F6, Ctrl+F4
    or ax, ax
    jnz msgloop
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

; A message to the client (bx), wParam ax, lParam 0: what SendMessage answered in ax.
to_client:
    push word [hclient]
    push bx
    push ax
    push word 0
    push word 0
    W16_CALL "USER", 111
    ret

; The frame's procedure, (hwnd, msg, wParam, lParam), Pascal, DS from AX.
frameproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_DESTROY
    je .destroy
    cmp ax, WM_COMMAND
    jne .default
    mov ax, [bp+10]
    cmp ax, 1
    je .tile
    cmp ax, 2
    je .cascade
    cmp ax, 3
    je .maximize
    cmp ax, 4
    je .restore
    jmp .default
.tile:
    mov bx, WM_MDITILE
    xor ax, ax
    call to_client
    jmp .zero
.cascade:
    mov bx, WM_MDICASCADE
    xor ax, ax
    call to_client
    jmp .zero
.maximize:
    mov bx, WM_MDIGETACTIVE
    xor ax, ax
    call to_client
    mov bx, WM_MDIMAXIMIZE
    call to_client
    jmp .zero
.restore:
    mov bx, WM_MDIGETACTIVE
    xor ax, ax
    call to_client
    mov bx, WM_MDIRESTORE
    call to_client
    jmp .zero
.destroy:
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
    jmp .zero
.default:
    push word [bp+14]
    push word [hclient]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 445            ; DefFrameProc
    jmp .ret
.zero:
    xor ax, ax
    xor dx, dx
.ret:
    pop ds
    pop bp
    retf 10

; A document's procedure: all of it DefMDIChildProc's.
docproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 447            ; DefMDIChildProc
    pop ds
    pop bp
    retf 10

W16_ENTRY start
W16_EXPORT 1, "FRAMEPROC", frameproc
W16_EXPORT 2, "DOCPROC", docproc

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
nCmdShow:   dw 0
hframe:     dw 0
hclient:    dw 0
hbar:       dw 0
hwinmenu:   dw 0

wc:
            dw 0
wc_proc:    W16_FARPTR frameproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
wc_hbr:     dw COLOR_APPWORKSPACE + 1
            dd 0
wc_name:    W16_FARPTR frameclass, 2

ccs:                                ; CLIENTCREATESTRUCT: the Window menu, the first document's id
ccs_menu:   dw 0
            dw 100

mcs1:                               ; MDICREATESTRUCT: class, title, owner, x, y, cx, cy, style, lParam
            W16_FARPTR docclass, 2
            W16_FARPTR one, 2
mcs1_owner: dw 0
            dw CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT
            dd 0, 0
mcs2:
            W16_FARPTR docclass, 2
            W16_FARPTR two, 2
mcs2_owner: dw 0
            dw CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT
            dd 0, 0

msg:
            dw 0, 0
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

item_text:  dw tile_text, cascade_text, max_text, restore_text

frameclass: db "MdiFrame", 0
docclass:   db "MdiDoc", 0
clientclass: db "MDICLIENT", 0
title:      db "MDI test", 0
one:        db "One", 0
two:        db "Two", 0
winmenu_text: db "&Window", 0
tile_text:  db "&Tile", 0
cascade_text: db "&Cascade", 0
max_text:   db "&Maximize", 0
restore_text: db "&Restore", 0
