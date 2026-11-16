; syscolor.asm -- SetSysColors read off the screen at once, as the Windows Tutorial's Windows Basics
; page 43 photographs Program Manager straight after it. Three windows paint a black square at client
; 20,20-60,60: the main one ("SysColor test", 600 x 440), and "Third" then "Other" owned by it inside
; it -- so their squares are on w16's screen, the main window's client area, and above it on 3.1.
; tools/tests/gui/w16_gui_test.py's `syscolor_erase` checks, in SYSCOLOR.INI:
;
;   - Before / After: the main window's square, then straight after SetSysColors(COLOR_ACTIVECAPTION,
;     its own colour) with no message taken -- SetSysColors erases nothing inside the call
;   - NoRaise: the main window's square straight after SetFocus(Other), the owned window already on
;     top: an activation that raises nothing erases nothing; CaptionB: Other's caption, lit at once
;   - Owner / Raised / Lowered: straight after SetFocus(Third), which raises it over Other, the squares
;     of the main window, Third and Other -- 3.11 erases every window owing an erase when an activation
;     raises one; Pending: GetUpdateRect on the main window then, 1 while its WM_PAINT is owed
;   - Painted: the main window's square once the messages are taken (black again)
;   - Refocus: Third owes an erase, SetFocus(NULL), then SetFocus back into Third, already active:
;     no activation, so its square stays black
;   Done=1 last.
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
    push word 600
    push word 440
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
    push word title3                ; a third, below the first: not in the activation
    push word 0
    push word 250
    call make_window
    mov [hwnd3], ax
    push word title2                ; a second beside them, made active
    push word 320
    push word 220
    call make_window
    mov [hwnd2], ax
    call drain
    push word [hwnd]
    W16_CALL "USER", 22             ; SetFocus: the square's window active again
    call drain

    mov ax, [hwnd]
    mov [sqwnd], ax
    mov si, key_before
    call read_square

    push word COLOR_ACTIVECAPTION   ; its own colour, so nothing is seen to change
    W16_CALL "USER", 180            ; GetSysColor
    mov [rgb], ax
    mov [rgb + 2], dx
    push word 1                     ; SetSysColors(1, &index, &rgb): every window owes an erase
    push ds
    push word index
    push ds
    push word rgb
    W16_CALL "USER", 181

    mov si, key_after               ; straight after, no message taken
    call read_square
    push word [hwnd2]
    W16_CALL "USER", 22             ; SetFocus(Other): activated, already on top -- nothing raised
    mov si, key_noraise
    call read_square
    mov ax, [hwnd2]
    mov [sqwnd], ax
    mov si, key_capb
    call read_caption
    push word [hwnd3]
    W16_CALL "USER", 22             ; SetFocus(Third): activated and raised over Other
    mov ax, [hwnd]
    mov [sqwnd], ax
    mov si, key_owner
    call read_square
    mov ax, [hwnd3]
    mov [sqwnd], ax
    mov si, key_raised
    call read_square
    mov ax, [hwnd2]
    mov [sqwnd], ax
    mov si, key_lowered
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
    mov ax, [hwnd]
    mov [sqwnd], ax
    mov si, key_painted
    call read_square

    push word [hwnd3]               ; the active window owes an erase, the focus is taken away,
    push word 0                     ; then given back to it: no activation, so nothing is erased
    push word 0
    push word 1
    W16_CALL "USER", 125            ; InvalidateRect(hwnd3, NULL, TRUE)
    push word 0
    W16_CALL "USER", 22             ; SetFocus(NULL)
    push word [hwnd3]
    W16_CALL "USER", 22             ; SetFocus(hwnd3)
    mov ax, [hwnd3]
    mov [sqwnd], ax
    mov si, key_refocus
    call read_square
    call drain
    mov si, key_done
    mov di, one_t
    call ini_write
    jmp msgloop

; CreateWindow of the class, titled by the word pushed first, at the x and y pushed after it,
; 300 x 200, owned by the first window, shown and painted; its handle in AX.
make_window:
    push bp
    mov bp, sp
    push ds
    push word classname
    push ds
    push word [bp+8]
    push word WS_OVERLAPPEDWINDOW >> 16
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word [bp+6]
    push word [bp+4]
    push word 300
    push word 200
    push word [hwnd]                ; owned by the first: above it on 3.1, on its screen under w16
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow
    or ax, ax
    jz fail
    push ax
    push ax
    push word 1
    W16_CALL "USER", 42             ; ShowWindow(SW_SHOWNORMAL)
    pop ax
    push ax
    push ax
    W16_CALL "USER", 124            ; UpdateWindow
    pop ax
    pop bp
    ret 6

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
    push word [sqwnd]               ; ClientToScreen(sqwnd, &pt)
    push ds
    push word pt
    W16_CALL "USER", 28
.at:
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

; The screen's pixel in [sqwnd]'s caption, 40 in and 10 down, to the INI under key SI.
read_caption:
    push word [sqwnd]               ; GetWindowRect(sqwnd, &rc)
    push ds
    push word rc
    W16_CALL "USER", 32
    mov ax, [rc]
    add ax, 40
    mov [pt], ax
    mov ax, [rc + 2]
    add ax, 10
    mov [pt + 2], ax
    push si
    jmp read_square.at

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
hwnd3:      dw 0
sqwnd:      dw 0
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
title3:     db "Third", 0
ini_app:    db "SysColor", 0
ini_file:   db "SYSCOLOR.INI", 0
key_before: db "Before", 0
key_after:  db "After", 0
key_noraise: db "NoRaise", 0
key_capb:   db "CaptionB", 0
key_owner:  db "Owner", 0
key_raised: db "Raised", 0
key_lowered: db "Lowered", 0
key_refocus: db "Refocus", 0
key_pending: db "Pending", 0
key_painted: db "Painted", 0
key_done:   db "Done", 0
pix_t:      db "000000", 0
one_t:      db "1", 0
zero_t:     db "0", 0
