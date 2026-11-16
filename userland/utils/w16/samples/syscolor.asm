; syscolor.asm -- SetSysColors read off the screen at once, as the Windows Tutorial's Windows Basics
; page 43 photographs Program Manager straight after it: a black square painted in the client area,
; then SetSysColors(COLOR_ACTIVECAPTION, its own colour). tools/tests/gui/w16_gui_test.py's
; `syscolor_erase` checks:
;
;   - Before: the screen's pixel in the square once it is painted (000000)
;   - After: the same pixel straight after SetSysColors, no message taken -- 3.11 erases every window's
;     client area inside the call and paints it later
;   - Pending: GetUpdateRect then, 1 when a WM_PAINT is owed
;   - Painted: the pixel once the messages are taken (000000 again); Done=1 last
;   All in SYSCOLOR.INI.
;
; Build: tools/gen/gen_ne.py.
;
;w16: module SYSCOLOR
;w16: description SetSysColors erase test
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
%define COLOR_ACTIVECAPTION 2
%define BLACK_BRUSH 4
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
    push word 300
    push word 200
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
    push ds                         ; a second top-level window beside it, made active
    push word classname
    push ds
    push word title2
    push word WS_OVERLAPPEDWINDOW >> 16
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word 320
    push word 220
    push word 300
    push word 200
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow
    or ax, ax
    jz fail
    mov [hwnd2], ax
    push ax
    push word 1
    W16_CALL "USER", 42             ; ShowWindow(SW_SHOWNORMAL)
    push word [hwnd2]
    W16_CALL "USER", 124            ; UpdateWindow
    call drain
    push word [hwnd]
    W16_CALL "USER", 22             ; SetFocus: the square's window active again
    call drain

    mov si, key_before
    call read_square

    push word COLOR_ACTIVECAPTION   ; its own colour, so nothing is seen to change
    W16_CALL "USER", 180            ; GetSysColor
    mov [rgb], ax
    mov [rgb + 2], dx
    push word 1                     ; SetSysColors(1, &index, &rgb)
    push ds
    push word index
    push ds
    push word rgb
    W16_CALL "USER", 181

    mov si, key_after               ; straight after, no message taken
    call read_square
    push word [hwnd2]
    W16_CALL "USER", 22             ; SetFocus: the other window active, the square's window behind it
    mov si, key_deact
    call read_square
    push word [hwnd]                ; GetUpdateRect(hwnd, &rc, FALSE)
    push ds
    push word rc
    push word 0
    W16_CALL "USER", 190
    mov di, zero_t
    or ax, ax
    jz .nopend
    mov di, one_t
.nopend:
    mov si, key_pending
    call ini_write

    call drain
    mov si, key_painted
    call read_square
    mov si, key_done
    mov di, one_t
    call ini_write
    jmp msgloop

; PeekMessage(&msg, 0, 0, 0, PM_REMOVE) and dispatch until there is none
drain:
    push ds
    push word msg
    push word 0
    push word 0
    push word 0
    push word 1
    W16_CALL "USER", 109
    or ax, ax
    jz .done
    push ds
    push word msg
    W16_CALL "USER", 114            ; DispatchMessage
    jmp drain
.done:
    ret

; The screen's pixel at client 30, 30 -- inside the square -- to the INI under key SI.
read_square:
    push si
    mov word [pt], 30
    mov word [pt + 2], 30
    push word [hwnd]                ; ClientToScreen(hwnd, &pt)
    push ds
    push word pt
    W16_CALL "USER", 28
    push word 0
    W16_CALL "USER", 66             ; GetDC(NULL): the screen
    mov [hdc], ax
    push ax
    push word [pt]
    push word [pt + 2]
    W16_CALL "GDI", 83              ; GetPixel
    mov [pix], ax
    mov [pix + 2], dx
    push word 0
    push word [hdc]
    W16_CALL "USER", 68             ; ReleaseDC
    mov al, [pix]                   ; COLORREF 00BBGGRR as RRGGBB
    mov di, pix_t
    call hex2
    mov al, [pix + 1]
    call hex2
    mov al, [pix + 2]
    call hex2
    pop si
    mov di, pix_t
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

; WritePrivateProfileString("SysColor", SI, DI, "SYSCOLOR.INI")
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

; LONG FAR PASCAL WndProc: WM_PAINT fills client 20,20-60,60 black.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    cmp word [bp+12], WM_DESTROY
    je .destroy
    cmp word [bp+12], WM_PAINT
    je .paint
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret
.paint:
    push word [bp+14]               ; BeginPaint(hwnd, &ps)
    push ds
    push word ps
    W16_CALL "USER", 39
    mov [phdc], ax
    push word BLACK_BRUSH
    W16_CALL "GDI", 87              ; GetStockObject
    push word [phdc]                ; FillRect(hdc, &square, black)
    push ds
    push word square
    push ax
    W16_CALL "USER", 81
    push word [bp+14]               ; EndPaint(hwnd, &ps)
    push ds
    push word ps
    W16_CALL "USER", 40
    xor ax, ax
    xor dx, dx
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

W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
hwnd2:      dw 0
hdc:        dw 0
phdc:       dw 0
pix:        dd 0
rgb:        dd 0
index:      dw COLOR_ACTIVECAPTION
pt:         dw 0, 0
rc:         dw 0, 0, 0, 0
square:     dw 20, 20, 60, 60
ps:         times 32 db 0

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

classname:  db "SysColorClass", 0
title:      db "SysColor test", 0
title2:     db "Other", 0
ini_app:    db "SysColor", 0
ini_file:   db "SYSCOLOR.INI", 0
key_before: db "Before", 0
key_after:  db "After", 0
key_deact:  db "Deactivated", 0
key_pending: db "Pending", 0
key_painted: db "Painted", 0
key_done:   db "Done", 0
pix_t:      db "000000", 0
one_t:      db "1", 0
zero_t:     db "0", 0
