; gdi.asm -- w16's test program for what Minesweeper-class programs use:
; resources (a string table and a bitmap), the global and local heaps, and
; GDI's objects -- a bitmap put with SetDIBitsToDevice and through a memory
; DC with BitBlt (SRCCOPY and NOTSRCCOPY), a pen line, a brush rectangle --
; and a timer. Everything lands at a fixed place in the client area, so
; tools/w16_gui_test.py checks it by pixel:
;
;   y  4        the string-table string (TextOut)
;   y 30..37    the bitmap at x 16 (SetDIBitsToDevice), x 40 (BitBlt copy),
;               x 64 (BitBlt NOTSRCCOPY: every colour inverted)
;   y 50        a red line, x 16..79
;   y 56..71    a blue rectangle with a black edge, x 16..47
;   y 80        text copied through a global block into a local one
;   x 100..107, y 50..57  green, once the timer has fired three times
;   x 120..127, y 50..57  magenta, when GDITEST.INI says [GdiTest] Square=1
;   x 140..219, y 30..69  a metafile played through MM_ANISOTROPIC at twice its
;               size: a red square (a NULL pen: its right and bottom left out)
;               and a green ellipse beside it
;   x 148..163, y 76..91  blue: a 32-pixel PatBlt at (140, 76) cut by
;               IntersectClipRect, inside SaveDC and RestoreDC
;   x 180..211, y 76..107 a blue triangle (Polygon), its edge black
;   x 224..225, y 76..87  the caret (CreateCaret, ShowCaret): black over the white
;
; and at start-up it writes [GdiTest] Written=yes to GDITEST.INI (drive
; C:'s WINDOWS folder), so both directions of the profile calls are seen.
;
; Build: tools/gen_ne.py.
;
;w16: module GDITEST
;w16: description GDI test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: string 1 Loaded from a string table.
;w16: bitmap 1 16x8 000000 ff0000 00ff00 0000ff ffffff ffff00 00ffff ff00ff
;w16: row 1 1111222233334444
;w16: row 1 1111222233334444
;w16: row 1 1111222233334444
;w16: row 1 1111222233334444
;w16: row 1 5555666677770000
;w16: row 1 5555666677770000
;w16: row 1 5555666677770000
;w16: row 1 5555666677770000

%include "ne16.inc"

%define WM_CREATE      0x0001
%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F
%define WM_TIMER       0x0113
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW   5
%define IDC_ARROW      32512
%define RT_BITMAP      2
%define CBM_INIT       4
%define BITS_OFFSET    40 + 16 * 4        ; BITMAPINFOHEADER and 16 RGBQUADs

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
    push word 240
    push word 150
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
    push word [hwnd]                ; CreateCaret(hwnd, NULL, 2, 12): a solid bar
    push word 0
    push word 2
    push word 12
    W16_CALL "USER", 163
    push word 224                   ; SetCaretPos(224, 76)
    push word 76
    W16_CALL "USER", 165
    push word 60000                 ; SetCaretBlinkTime: on for a minute, so a check sees it on
    W16_CALL "USER", 168
    push word [hwnd]
    W16_CALL "USER", 167            ; ShowCaret

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

wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_CREATE
    je .create
    cmp ax, WM_PAINT
    je .paint
    cmp ax, WM_TIMER
    je .timer
    cmp ax, WM_DESTROY
    je .destroy
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret

.create:
    ; A global block, written, then copied into a local block: both heaps.
    push word 0x0002                ; GMEM_MOVEABLE
    push word 0
    push word 32
    W16_CALL "KERNEL", 15           ; GlobalAlloc(flags, 32)
    mov [hglobal], ax
    push ax
    W16_CALL "KERNEL", 18           ; GlobalLock -> DX:AX
    push dx                         ; lstrcpy(global, heaps_text)
    push ax
    push ds
    push word heaps_text
    W16_CALL "KERNEL", 88
    push word 0x0040                ; LMEM_ZEROINIT
    push word 32
    W16_CALL "KERNEL", 5            ; LocalAlloc -> a DGROUP offset
    mov [hlocal], ax
    push ds                         ; lstrcpy(local, global)
    push ax
    push word [hglobal]
    push word 0
    W16_CALL "KERNEL", 88
    push word [hglobal]
    W16_CALL "KERNEL", 19           ; GlobalUnlock
    ; The string table.
    push word [hInstance]
    push word 1
    push ds
    push word strbuf
    push word 64
    W16_CALL "USER", 176            ; LoadString
    mov [strlen_], ax
    ; The INI file, both ways.
    push ds                         ; WritePrivateProfileString("GdiTest", "Written", "yes", "GDITEST.INI")
    push word ini_app
    push ds
    push word ini_written
    push ds
    push word ini_yes
    push ds
    push word ini_file
    W16_CALL "KERNEL", 129
    push ds                         ; GetPrivateProfileInt("GdiTest", "Square", 0, "GDITEST.INI")
    push word ini_app
    push ds
    push word ini_square
    push word 0
    push ds
    push word ini_file
    W16_CALL "KERNEL", 127
    mov [square], ax
    ; The timer: 100 ms, to the window.
    push word [bp+14]
    push word 1
    push word 100
    push word 0
    push word 0
    W16_CALL "USER", 10             ; SetTimer
    jmp .zero

.timer:
    inc word [ticks]
    cmp word [ticks], 3
    jne .zero
    push word [bp+14]
    push word 1
    W16_CALL "USER", 12             ; KillTimer
    push word [bp+14]
    push word 0
    push word 0
    push word 1
    W16_CALL "USER", 125            ; InvalidateRect(hwnd, NULL, TRUE)
    jmp .zero

.paint:
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 39             ; BeginPaint
    mov [hdc], ax

    push ax                         ; TextOut(hdc, 4, 4, strbuf, len)
    push word 4
    push word 4
    push ds
    push word strbuf
    push word [strlen_]
    W16_CALL "GDI", 33

    ; The bitmap resource, locked: DX:AX is its BITMAPINFOHEADER.
    push word [hInstance]
    push word 0
    push word 1
    push word 0
    push word RT_BITMAP
    W16_CALL "KERNEL", 60           ; FindResource(hInst, 1, RT_BITMAP)
    push word [hInstance]
    push ax
    W16_CALL "KERNEL", 61           ; LoadResource
    push ax
    W16_CALL "KERNEL", 62           ; LockResource -> DX:AX
    mov [bmi_seg], dx

    push word [hdc]                 ; SetDIBitsToDevice(hdc, 16, 30, 16, 8, 0, 0, 0, 8,
    push word 16                    ;   bits, bmi, DIB_RGB_COLORS)
    push word 30
    push word 16
    push word 8
    push word 0
    push word 0
    push word 0
    push word 8
    push word [bmi_seg]
    push word BITS_OFFSET
    push word [bmi_seg]
    push word 0
    push word 0
    W16_CALL "GDI", 443

    push word [hdc]                 ; CreateDIBitmap(hdc, bmih, CBM_INIT, bits, bmi, 0)
    push word [bmi_seg]
    push word 0
    push word 0
    push word CBM_INIT
    push word [bmi_seg]
    push word BITS_OFFSET
    push word [bmi_seg]
    push word 0
    push word 0
    W16_CALL "GDI", 442
    mov [hbm], ax
    push word [hdc]
    W16_CALL "GDI", 52              ; CreateCompatibleDC
    mov [mdc], ax
    push ax
    push word [hbm]
    W16_CALL "GDI", 45              ; SelectObject(mdc, hbm)
    mov [oldbm], ax

    push word [hdc]                 ; BitBlt(hdc, 40, 30, 16, 8, mdc, 0, 0, SRCCOPY)
    push word 40
    push word 30
    push word 16
    push word 8
    push word [mdc]
    push word 0
    push word 0
    push word 0x00CC
    push word 0x0020
    W16_CALL "GDI", 34
    push word [hdc]                 ; ... and at 64, NOTSRCCOPY
    push word 64
    push word 30
    push word 16
    push word 8
    push word [mdc]
    push word 0
    push word 0
    push word 0x0033
    push word 0x0008
    W16_CALL "GDI", 34

    push word [mdc]
    push word [oldbm]
    W16_CALL "GDI", 45              ; the default bitmap back
    push word [mdc]
    W16_CALL "GDI", 68              ; DeleteDC
    push word [hbm]
    W16_CALL "GDI", 69              ; DeleteObject

    ; A red line with a pen of our own.
    push word 0                     ; CreatePen(PS_SOLID, 1, RGB(255,0,0))
    push word 1
    push word 0
    push word 0x00FF
    W16_CALL "GDI", 61
    mov [hpen], ax
    push word [hdc]
    push ax
    W16_CALL "GDI", 45              ; SelectObject(hdc, pen)
    mov [oldpen], ax
    push word [hdc]
    push word 16
    push word 50
    W16_CALL "GDI", 20              ; MoveTo
    push word [hdc]
    push word 80
    push word 50
    W16_CALL "GDI", 19              ; LineTo (the end point is not drawn)
    push word [hdc]
    push word [oldpen]
    W16_CALL "GDI", 45              ; the black pen back
    push word [hpen]
    W16_CALL "GDI", 69

    ; A blue rectangle, its edge in the DC's black pen.
    push word 0x00FF                ; CreateSolidBrush(RGB(0,0,255))
    push word 0x0000
    W16_CALL "GDI", 66
    mov [hbrush], ax
    push word [hdc]
    push ax
    W16_CALL "GDI", 45
    mov [oldbrush], ax
    push word [hdc]
    push word 16
    push word 56
    push word 48
    push word 72
    W16_CALL "GDI", 27              ; Rectangle

    ; Text that went through both heaps.
    push word [hdc]
    push word 4
    push word 80
    push ds
    push word [hlocal]
    push word heaps_len
    W16_CALL "GDI", 33

    ; A metafile in a global block, played at twice its size: the window its
    ; records set (40 x 20) onto a viewport of 80 x 40 at (140, 30).
    push word 0x0002                ; GlobalAlloc(GMEM_MOVEABLE, wmf_len)
    push word 0
    push word wmf_len
    W16_CALL "KERNEL", 15
    mov [hmf], ax
    push ax
    W16_CALL "KERNEL", 18           ; GlobalLock -> DX:AX
    mov es, dx
    mov di, ax
    mov si, wmf
    mov cx, wmf_len
    cld
    rep movsb
    push word [hmf]
    W16_CALL "KERNEL", 19           ; GlobalUnlock
    push word [hdc]
    push word 8
    W16_CALL "GDI", 3               ; SetMapMode(MM_ANISOTROPIC)
    push word [hdc]
    push word 140
    push word 30
    W16_CALL "GDI", 13              ; SetViewportOrg
    push word [hdc]
    push word 80
    push word 40
    W16_CALL "GDI", 14              ; SetViewportExt
    push word [hdc]
    push word [hmf]
    W16_CALL "GDI", 123             ; PlayMetaFile
    push word [hdc]
    push word 1
    W16_CALL "GDI", 3               ; SetMapMode(MM_TEXT)
    push word [hdc]
    push word 0
    push word 0
    W16_CALL "GDI", 13              ; SetViewportOrg(0, 0)
    push word [hmf]
    W16_CALL "KERNEL", 17           ; GlobalFree

    ; The clip: kept by SaveDC, cut to 16 x 16, a wider PatBlt, then back.
    push word [hdc]
    W16_CALL "GDI", 30              ; SaveDC
    push word [hdc]
    push word 148
    push word 76
    push word 164
    push word 92
    W16_CALL "GDI", 22              ; IntersectClipRect
    push word [hdc]                 ; PatBlt(hdc, 140, 76, 32, 32, PATCOPY): the blue brush
    push word 140
    push word 76
    push word 32
    push word 32
    push word 0x00F0
    push word 0x0021
    W16_CALL "GDI", 29
    push word [hdc]
    push word -1
    W16_CALL "GDI", 39              ; RestoreDC(-1)

    ; A blue triangle with a black edge.
    push word [hdc]
    push ds
    push word triangle
    push word 3
    W16_CALL "GDI", 36              ; Polygon

    ; Magenta, when the INI file said so.
    cmp word [square], 1
    jne .nosquare
    push word 0x00FF                ; CreateSolidBrush(RGB(255,0,255))
    push word 0x00FF
    W16_CALL "GDI", 66
    mov [hsquare], ax
    push word [hdc]
    push ax
    W16_CALL "GDI", 45
    push word [hdc]                 ; PatBlt(hdc, 120, 50, 8, 8, PATCOPY)
    push word 120
    push word 50
    push word 8
    push word 8
    push word 0x00F0
    push word 0x0021
    W16_CALL "GDI", 29
    push word [hdc]
    push word [hbrush]
    W16_CALL "GDI", 45              ; the blue one back, for what follows
    push word [hsquare]
    W16_CALL "GDI", 69
.nosquare:
    ; Green, once the timer has fired three times.
    cmp word [ticks], 3
    jb .nogreen
    push word 0x0000                ; CreateSolidBrush(RGB(0,255,0))
    push word 0xFF00
    W16_CALL "GDI", 66
    mov [hgreen], ax
    push word [hdc]
    push ax
    W16_CALL "GDI", 45
    push word [hdc]                 ; PatBlt(hdc, 100, 50, 8, 8, PATCOPY)
    push word 100
    push word 50
    push word 8
    push word 8
    push word 0x00F0
    push word 0x0021
    W16_CALL "GDI", 29
    push word [hdc]
    push word [oldbrush]
    W16_CALL "GDI", 45
    push word [hgreen]
    W16_CALL "GDI", 69
    jmp .endpaint
.nogreen:
    push word [hdc]
    push word [oldbrush]
    W16_CALL "GDI", 45
.endpaint:
    push word [hbrush]
    W16_CALL "GDI", 69
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint
    jmp .zero

.destroy:
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
.zero:
    xor ax, ax
    xor dx, dx
.ret:
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
hdc:        dw 0
mdc:        dw 0
hbm:        dw 0
oldbm:      dw 0
hpen:       dw 0
oldpen:     dw 0
hbrush:     dw 0
hgreen:     dw 0
oldbrush:   dw 0
bmi_seg:    dw 0
hglobal:    dw 0
hlocal:     dw 0
ticks:      dw 0
square:     dw 0
hsquare:    dw 0
strlen_:    dw 0

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
strbuf:     times 64 db 0

hmf:        dw 0
triangle:   dw 180, 76, 212, 76, 180, 108

; A memory metafile: a 9-word METAHEADER, then records (a DWORD size in words,
; the function, its arguments last first).
wmf:
            dw 1, 9, 0x0300
            dd wmf_words
            dw 3                    ; objects: two brushes and a pen
            dd 8
            dw 0
            dd 5
            dw 0x020C, 20, 40       ; SetWindowExt(40, 20)
            dd 7
            dw 0x02FC, 0            ; CreateBrushIndirect(BS_SOLID, red)
            dd 0x000000FF
            dw 0
            dd 4
            dw 0x012D, 0            ; SelectObject(0)
            dd 8
            dw 0x02FA, 5, 0, 0      ; CreatePenIndirect(PS_NULL)
            dd 0
            dd 4
            dw 0x012D, 1            ; SelectObject(1)
            dd 7
            dw 0x041B, 20, 20, 0, 0 ; Rectangle(0, 0, 20, 20)
            dd 7
            dw 0x02FC, 0            ; CreateBrushIndirect(BS_SOLID, green)
            dd 0x0000FF00
            dw 0
            dd 4
            dw 0x012D, 2            ; SelectObject(2)
            dd 7
            dw 0x0418, 20, 40, 0, 20 ; Ellipse(20, 0, 40, 20)
            dd 3
            dw 0                    ; META_EOF
wmf_len     equ $ - wmf
wmf_words   equ wmf_len / 2

classname:  db "GdiTestClass", 0
title:      db "GDI test (Win16)", 0
heaps_text: db "Through both heaps.", 0
heaps_len   equ $ - heaps_text - 1
ini_app:    db "GdiTest", 0
ini_written: db "Written", 0
ini_yes:    db "yes", 0
ini_square: db "Square", 0
ini_file:   db "GDITEST.INI", 0
