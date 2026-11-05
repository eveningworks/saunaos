; fullwin.asm -- a second top-level window and the screen under it, as the Windows Tutorial's Windows
; Basics uses them. tools/tests/gui/w16_gui_test.py's `fullwin` checks each by pixel or by FULLWIN.INI:
;
;   - FrameRgn, 1 by 1 round (250,100)-(290,140) and 2 by 2 round (300,100)-(340,140), in black:
;     the edge pixel black and the one just inside it the background (a 1 by 1 frame once drew nothing)
;   - icon 1 drawn at (250,160): its top-left quarter dark magenta, its top-right dark grey, as the
;     VGA showed them (A8,57,A8 and 87,88,8F) -- they were both 80,80,80
;   - icon 2 at (300,160) has an 8-colour picture (yellow) ahead of a 16-colour one (dark cyan): 3.1 at
;     VGA chose the 16-colour one
;   - a second WS_OVERLAPPEDWINDOW ("Second") over the main window, its square red; on the timer it is
;     painted green and the screen under it copied into the main window at (250,10) with no wait
;     between -- the copy is green
;   - then maximized (SC_MAXIMIZE through DefWindowProc), hidden, and SW_SHOWNORMALed: GetWindowRect
;     gives the rect it had before (Restored=1)
;   - then a WH_JOURNALPLAYBACK hook plays a click at (300,220) of the main window and, on its last
;     HC_SKIP, unhooks itself and posts WM_USER: the click's WM_LBUTTONUP comes first (UpFirst=1)
;
; Build: tools/gen/gen_ne.py.
;
;w16: module FULLWIN
;w16: description Full windows test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: icon 1 16
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: irow 1 55555555555555558888888888888888
;w16: icon 2 8
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: irow 2 BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
;w16: icon 2 16
;w16: irow 2 66666666666666666666666666666666
;w16: irow 2 66666666666666666666666666666666
;w16: irow 2 66666666666666666666666666666666
;w16: irow 2 66666666666666666666666666666666
;w16: irow 2 66666666666666666666666666666666
;w16: irow 2 66666666666666666666666666666666
;w16: irow 2 66666666666666666666666666666666
;w16: irow 2 66666666666666666666666666666666

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F
%define WM_TIMER       0x0113
%define WM_SYSCOMMAND  0x0112
%define WM_LBUTTONDOWN 0x0201
%define WM_LBUTTONUP   0x0202
%define WM_USER        0x0400
%define SC_MAXIMIZE    0xF030
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define WS_VISIBLE_HI  0x1000
%define COLOR_WINDOW   5
%define IDC_ARROW      32512
%define BLACK_BRUSH    4
%define WH_JOURNALPLAYBACK 1
%define HC_GETNEXT     1
%define HC_SKIP        2

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
    W16_CALL "USER", 57             ; RegisterClass: the main window's
    or ax, ax
    jz fail
    mov word [wc_name], secondclass
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass: the second window's
    or ax, ax
    jz fail

    push word [hInstance]
    push word 0
    push word 1
    W16_CALL "USER", 174            ; LoadIcon(hInstance, 1)
    mov [hicon1], ax
    push word [hInstance]
    push word 0
    push word 2
    W16_CALL "USER", 174            ; LoadIcon(hInstance, 2)
    mov [hicon2], ax

    push ds
    push word mainclass
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
    W16_CALL "USER", 41             ; CreateWindow: the main window
    or ax, ax
    jz fail
    mov [hwnd], ax
    push ax
    push word [nCmdShow]
    W16_CALL "USER", 42             ; ShowWindow
    push word [hwnd]
    W16_CALL "USER", 124            ; UpdateWindow

    mov word [pt], 20               ; the second window at the main client's (20, 60)
    mov word [pt+2], 60
    push word [hwnd]
    push ds
    push word pt
    W16_CALL "USER", 28             ; ClientToScreen
    push ds
    push word secondclass
    push ds
    push word title2
    push word (WS_OVERLAPPEDWINDOW >> 16) | WS_VISIBLE_HI
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word [pt]
    push word [pt+2]
    push word 160
    push word 100
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: the second top-level window
    or ax, ax
    jz fail
    mov [hsecond], ax
    push ax
    W16_CALL "USER", 124            ; UpdateWindow

    push word [hwnd]
    push word 1
    push word 700
    push word 0
    push word 0
    W16_CALL "USER", 10             ; SetTimer(hwnd, 1, 700, NULL)

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

; WritePrivateProfileString("FullWin", SI, DI, "FULLWIN.INI")
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

; A RECT at SI as 16 hex digits at DI (left, top, right, bottom)
rect_hex:
    push ds
    pop es
    mov cx, 4
.r:
    lodsw
    push cx
    call hex16
    pop cx
    loop .r
    mov byte [di], 0
    ret

hex16:
    push ax
    mov al, ah
    call hex8
    pop ax
hex8:
    push ax
    shr al, 4
    call .digit
    pop ax
.digit:
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .put
    add al, 'A' - '9' - 1
.put:
    stosb
    ret

; A brush's colour into a rect: BX the brush, the rect at rc
fill_rc:
    push word [hdc]
    push ds
    push word rc
    push bx
    W16_CALL "USER", 81             ; FillRect
    ret

; FrameRgn(hdc, CreateRectRgn(rc), BLACK_BRUSH, CX, CX)
frame_rc:
    push cx
    push word [rc]
    push word [rc+2]
    push word [rc+4]
    push word [rc+6]
    W16_CALL "GDI", 64              ; CreateRectRgn
    pop cx
    mov [hrgn], ax
    push word BLACK_BRUSH
    W16_CALL "GDI", 87              ; GetStockObject
    push word [hdc]
    push word [hrgn]
    push ax
    push cx
    push cx
    W16_CALL "GDI", 41              ; FrameRgn
    push word [hrgn]
    W16_CALL "GDI", 69              ; DeleteObject
    ret

; The one procedure of both classes, (hwnd, msg, wParam, lParam), Pascal, DS from AX.
wndproc:
    push bp
    mov bp, sp
    push ds
    push si
    push di
    mov ds, ax
    mov ax, [bp+14]
    cmp ax, [hsecond]
    jne .main
    cmp word [bp+12], WM_PAINT
    jne .default
    push word [bp+14]               ; the second window's square: red, or green once [green] is set
    push ds
    push word ps
    W16_CALL "USER", 39             ; BeginPaint
    mov [hdc], ax
    push word 0x0000
    push word 0x00FF                ; RGB(255,0,0)
    cmp word [green], 0
    je .brush
    add sp, 2
    push word 0xFF00                ; RGB(0,255,0)
.brush:
    W16_CALL "GDI", 66              ; CreateSolidBrush
    mov [hbrush], ax
    mov word [rc], 0
    mov word [rc+2], 0
    mov word [rc+4], 16
    mov word [rc+6], 16
    mov bx, ax
    call fill_rc
    push word [hbrush]
    W16_CALL "GDI", 69              ; DeleteObject
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint
    jmp .zero

.main:
    mov ax, [bp+12]
    cmp ax, WM_PAINT
    je .paint
    cmp ax, WM_TIMER
    je .timer
    cmp ax, WM_LBUTTONUP
    je .up
    cmp ax, WM_USER
    je .user
    cmp ax, WM_DESTROY
    jne .default
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
    jmp .zero

.paint:
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 39             ; BeginPaint
    mov [hdc], ax
    mov word [rc], 250
    mov word [rc+2], 100
    mov word [rc+4], 290
    mov word [rc+6], 140
    mov cx, 1
    call frame_rc
    mov word [rc], 300
    mov word [rc+4], 340
    mov cx, 2
    call frame_rc
    push word [hdc]
    push word 250
    push word 160
    push word [hicon1]
    W16_CALL "USER", 84             ; DrawIcon
    push word [hdc]
    push word 300
    push word 160
    push word [hicon2]
    W16_CALL "USER", 84             ; DrawIcon
    push word [bp+14]
    push ds
    push word ps
    W16_CALL "USER", 40             ; EndPaint
    jmp .zero

.timer:
    push word [hwnd]
    push word 1
    W16_CALL "USER", 12             ; KillTimer
    ; Painted green and read back at once: no message waited for between the two.
    mov word [green], 1
    push word [hsecond]
    push word 0
    push word 0
    push word 1
    W16_CALL "USER", 125            ; InvalidateRect(second, NULL, TRUE)
    push word [hsecond]
    W16_CALL "USER", 124            ; UpdateWindow(second): its WM_PAINT now
    push word [hwnd]
    W16_CALL "USER", 66             ; GetDC(main)
    mov [hdc], ax
    mov word [pt], 4                ; the second window's square, in the main window's coordinates
    mov word [pt+2], 4
    push word [hsecond]
    push ds
    push word pt
    W16_CALL "USER", 28             ; ClientToScreen
    push word [hwnd]
    push ds
    push word pt
    W16_CALL "USER", 29             ; ScreenToClient
    push word [hdc]                 ; BitBlt(main, 250, 10, 8, 8, main, pt, SRCCOPY)
    push word 250
    push word 10
    push word 8
    push word 8
    push word [hdc]
    push word [pt]
    push word [pt+2]
    push word 0x00CC
    push word 0x0020
    W16_CALL "GDI", 34
    push word [hwnd]
    push word [hdc]
    W16_CALL "USER", 68             ; ReleaseDC

    push word [hsecond]
    push ds
    push word before
    W16_CALL "USER", 32             ; GetWindowRect: as it was made
    push word [hsecond]
    push word WM_SYSCOMMAND
    push word SC_MAXIMIZE
    push word 0
    push word 0
    W16_CALL "USER", 111            ; SendMessage(second, WM_SYSCOMMAND, SC_MAXIMIZE, 0)
    push word [hsecond]
    push word 0
    W16_CALL "USER", 42             ; ShowWindow(SW_HIDE)
    push word [hsecond]
    push word 1
    W16_CALL "USER", 42             ; ShowWindow(SW_SHOWNORMAL): restored
    push word [hsecond]
    push ds
    push word after
    W16_CALL "USER", 32             ; GetWindowRect
    mov si, before
    mov di, after
    mov cx, 4
    push ds
    pop es
    repe cmpsw
    mov byte [restored_t], '0'
    jne .written
    mov byte [restored_t], '1'
.written:
    mov si, key_restored
    mov di, restored_t
    call ini_write
    mov si, before
    mov di, rect_t
    call rect_hex
    mov si, key_before
    mov di, rect_t
    call ini_write
    mov si, after
    mov di, rect_t
    call rect_hex
    mov si, key_after
    mov di, rect_t
    call ini_write

    mov word [pt], 300              ; the journal's click, at the main client's (300, 220)
    mov word [pt+2], 220
    push word [hwnd]
    push ds
    push word pt
    W16_CALL "USER", 28             ; ClientToScreen
    mov word [played], 0
    push word WH_JOURNALPLAYBACK
    W16_PUSH_FAR hookproc, 1
    W16_CALL "USER", 121            ; SetWindowsHook
    jmp .zero

.up:
    cmp word [posted_seen], 0
    jne .zero
    mov word [up_seen], 1
    jmp .zero

.user:
    mov word [posted_seen], 1
    mov byte [up_t], '0'
    cmp word [up_seen], 0
    je .say
    mov byte [up_t], '1'
.say:
    mov si, key_up
    mov di, up_t
    call ini_write
    jmp .zero

.default:
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    jmp .ret
.zero:
    xor ax, ax
    xor dx, dx
.ret:
    pop di
    pop si
    pop ds
    pop bp
    retf 10

; The journal hook, (code, wParam, lParam), Pascal, DS from AX: a press, then a release, at pt.
hookproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, HC_GETNEXT
    je .next
    cmp ax, HC_SKIP
    jne .done
    inc word [played]
    cmp word [played], 2
    jne .done
    push word WH_JOURNALPLAYBACK    ; the last one moved past: off, and say so after it
    W16_PUSH_FAR hookproc, 1
    W16_CALL "USER", 234            ; UnhookWindowsHook
    push word [hwnd]
    push word WM_USER
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 110            ; PostMessage(main, WM_USER, 0, 0)
    jmp .done
.next:
    les bx, [bp+6]                  ; the EVENTMSG: message, paramL, paramH, time
    mov ax, WM_LBUTTONDOWN
    cmp word [played], 0
    je .msg
    mov ax, WM_LBUTTONUP
.msg:
    mov [es:bx], ax
    mov ax, [pt]
    mov [es:bx+2], ax
    mov ax, [pt+2]
    mov [es:bx+4], ax
    mov word [es:bx+6], 0
    mov word [es:bx+8], 0
.done:
    xor ax, ax                      ; no wait before it
    xor dx, dx
    pop ds
    pop bp
    retf 8

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc
W16_EXPORT 2, "HOOKPROC", hookproc

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
nCmdShow:   dw 0
hwnd:       dw 0
hsecond:    dw 0
hdc:        dw 0
hbrush:     dw 0
hrgn:       dw 0
hicon1:     dw 0
hicon2:     dw 0
green:      dw 0
played:     dw 0
up_seen:    dw 0
posted_seen: dw 0
pt:         dw 0, 0
rc:         dw 0, 0, 0, 0
before:     dw 0, 0, 0, 0
after:      dw 0, 0, 0, 0

wc:
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
            dw COLOR_WINDOW + 1
            dd 0
wc_name:    W16_FARPTR mainclass, 2

msg:
            dw 0, 0
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

ps:         times 32 db 0

mainclass:  db "FullWinMain", 0
secondclass: db "FullWinSecond", 0
title:      db "Full windows test", 0
title2:     db "Second", 0
ini_app:    db "FullWin", 0
ini_file:   db "FULLWIN.INI", 0
key_restored: db "Restored", 0
key_before: db "Before", 0
key_after:  db "After", 0
key_up:     db "UpFirst", 0
restored_t: db "0", 0
up_t:       db "0", 0
rect_t:     times 20 db 0
