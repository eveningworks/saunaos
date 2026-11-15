; menukeys.asm -- menus from the keyboard, as the Windows Tutorial's Windows Basics opens Write's File
; menu. tools/tests/gui/w16_gui_test.py's `menukeys` checks each by MENUKEYS.INI or by pixel:
;
;   - a main window ("Menu keys test") with a class menu: File (New 101, Open 102), Edit (Copy 103)
;   - a second WS_OVERLAPPEDWINDOW ("Keys") with its own class menu: File (New 201, Open 202), View
;     (Zoom 203), given the focus
;   - 1.5 s later, the windows up, a WH_JOURNALPLAYBACK hook plays Alt down and up, Down twice and Enter, then unhooks: 3.1's
;     DefWindowProc made Alt alone SC_KEYMENU, the bar of the window with the focus lit its first
;     title, Down dropped File and moved to Open, and Enter chose it -- WM_COMMAND 202 to the second
;     window (Played=2). The second window's own procedure saw the played Alt as WM_SYSKEYDOWN first
;     (SysKey=1)
;   - a command to the main window is Typed=<id - 100>, for the keys the test presses itself
;
; Build: tools/gen/gen_ne.py.
;
;w16: module MENUKEYS
;w16: description Menu keys test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: menu 1
;w16: popup 1 "&File"
;w16: item 1 101 "&New"
;w16: item 1 102 "&Open"
;w16: popup 1 "&Edit"
;w16: item 1 103 "&Copy"
;w16: menu 2
;w16: popup 2 "&File"
;w16: item 2 201 "&New"
;w16: item 2 202 "&Open"
;w16: popup 2 "&View"
;w16: item 2 203 "&Zoom"

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_SYSKEYDOWN  0x0104
%define WM_COMMAND     0x0111
%define WM_TIMER       0x0113
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define WS_VISIBLE_HI  0x1000
%define COLOR_WINDOW   5
%define IDC_ARROW      32512
%define VK_MENU        0x12
%define WH_JOURNALPLAYBACK 1
%define HC_GETNEXT     1
%define HC_SKIP        2
%define NPLAYED        8

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
    W16_CALL "USER", 57             ; RegisterClass: the main window's, menu 1
    or ax, ax
    jz fail
    mov word [wc_name], secondclass
    mov word [wc_menu], 2
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass: the second window's, menu 2
    or ax, ax
    jz fail

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

    mov word [pt], 40               ; the second window at the main client's (40, 80)
    mov word [pt+2], 80
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
    push word 220
    push word 120
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
    push word [hsecond]
    W16_CALL "USER", 22             ; SetFocus: the keys are its

    push word [hwnd]
    push word 1
    push word 1500
    push word 0
    push word 0
    W16_CALL "USER", 10             ; SetTimer(hwnd, 1, 1500, NULL): the journal once the windows are up

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

; WritePrivateProfileString("MenuKeys", SI, DI, "MENUKEYS.INI")
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

; The one procedure of both classes, (hwnd, msg, wParam, lParam), Pascal, DS from AX.
wndproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_SYSKEYDOWN
    je .syskey
    cmp ax, WM_COMMAND
    je .command
    cmp ax, WM_TIMER
    je .timer
    cmp ax, WM_DESTROY
    je .destroy
.default:
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107            ; DefWindowProc
    pop ds
    pop bp
    retf 10

.syskey:                            ; the second window's played Alt: seen here first, then DefWindowProc's
    mov ax, [bp+14]
    cmp ax, [hsecond]
    jne .default
    cmp word [bp+10], VK_MENU
    jne .default
    mov si, key_syskey
    mov di, one_t
    call ini_write
    jmp .default

.command:
    mov ax, [bp+10]                 ; the command: 201.. to the second window, 101.. to the main one
    mov si, key_played
    sub ax, 200
    mov bx, [bp+14]
    cmp bx, [hsecond]
    je .say
    mov si, key_typed
    add ax, 100
.say:
    add al, '0'
    mov [num_t], al
    mov di, num_t
    call ini_write
    jmp .zero

.timer:
    push word [hwnd]
    push word 1
    W16_CALL "USER", 12             ; KillTimer
    mov word [played], 0
    push word WH_JOURNALPLAYBACK
    W16_PUSH_FAR hookproc, 1
    W16_CALL "USER", 121            ; SetWindowsHook
    jmp .zero

.destroy:
    mov ax, [bp+14]
    cmp ax, [hwnd]
    jne .zero
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage(0)
.zero:
    xor ax, ax
    xor dx, dx
    pop ds
    pop bp
    retf 10

; The journal hook, (code, wParam, lParam), Pascal, DS from AX: the keys in `events`, one each
; HC_GETNEXT, the next after each HC_SKIP; off after the last.
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
    cmp word [played], NPLAYED
    jne .done
    push word WH_JOURNALPLAYBACK
    W16_PUSH_FAR hookproc, 1
    W16_CALL "USER", 234            ; UnhookWindowsHook
    jmp .done
.next:
    les bx, [bp+6]                  ; the EVENTMSG: message, paramL (VK | scan << 8), paramH, time
    mov si, [played]
    shl si, 2
    mov ax, [events + si]
    mov [es:bx], ax
    mov ax, [events + si + 2]
    mov [es:bx+2], ax
    mov word [es:bx+4], 1
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
played:     dw 0
pt:         dw 0, 0

; Alt down and up, Down, Down, Enter: (message, VK | scan << 8)
events:
            dw 0x0104, 0x3812       ; WM_SYSKEYDOWN VK_MENU
            dw 0x0105, 0x3812       ; WM_SYSKEYUP VK_MENU
            dw 0x0100, 0x5028       ; WM_KEYDOWN VK_DOWN
            dw 0x0101, 0x5028       ; WM_KEYUP VK_DOWN
            dw 0x0100, 0x5028
            dw 0x0101, 0x5028
            dw 0x0100, 0x1C0D       ; WM_KEYDOWN VK_RETURN
            dw 0x0101, 0x1C0D

wc:
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
            dw COLOR_WINDOW + 1
wc_menu:    dw 1, 0                 ; MAKEINTRESOURCE: the class's menu by number
wc_name:    W16_FARPTR mainclass, 2

msg:
            dw 0, 0
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

mainclass:  db "MenuKeysMain", 0
secondclass: db "MenuKeysSecond", 0
title:      db "Menu keys test", 0
title2:     db "Keys", 0
ini_app:    db "MenuKeys", 0
ini_file:   db "MENUKEYS.INI", 0
key_syskey: db "SysKey", 0
key_played: db "Played", 0
key_typed:  db "Typed", 0
one_t:      db "1", 0
num_t:      db "0", 0
