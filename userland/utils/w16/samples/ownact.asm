; ownact.asm -- activation between a program's own windows, the Windows Tutorial's shape: a captionless
; WS_POPUP ("Owner", 0,0 400x300) with a child filling it, and a WS_OVERLAPPEDWINDOW ("Owned", at
; 150,80 220x140) made with that CHILD as its parent -- 3.1 made the popup its owner. Every half
; second OWNACT.INI's [Act] gets Active= (GetActiveWindow) and Focus= (GetFocus), beside Owner=,
; Client= and Owned=. Measured on the 3.11 reference (2026-11-08): a click on the owner makes it the
; active window and the owned window's caption inactive -- even while that window swallows WM_NCPAINT.
; A RIGHT click on the owner asks, 4 s later, ShowWindow(owned, SW_SHOW), which 3.1 made activate a
; window already shown (the Tutorial gives Write the focus back so): w16_gui_test's `ownact` checks it
; does, and that it takes nothing from another program focused meanwhile.
;
; Build: tools/gen/gen_ne.py.
;
;w16: module OWNACT
;w16: description Owner activation probe
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_TIMER       0x0113
%define WS_POPUP_VISIBLE_HI 0x9000
%define WS_CHILD_VISIBLE_HI 0x5000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define WS_VISIBLE_HI  0x1000
%define COLOR_WINDOW   5
%define COLOR_BTNFACE  15
%define IDC_ARROW      32512

; ---------------------------------------------------------------- code
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
    W16_CALL "USER", 57             ; RegisterClass: the owner's (white)
    or ax, ax
    jz fail
    mov word [wc_name], clientclass
    mov word [wc_bg], COLOR_BTNFACE + 1
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass: its child's (grey)
    mov word [wc_name], ownedclass
    mov word [wc_bg], COLOR_WINDOW + 1
    push ds
    push word wc
    W16_CALL "USER", 57             ; RegisterClass: the owned window's

    push ds
    push word ownerclass
    push ds
    push word ownertitle
    push word WS_POPUP_VISIBLE_HI
    push word 0
    push word 0
    push word 0
    push word 400
    push word 300
    push word 0
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: the captionless popup
    or ax, ax
    jz fail
    mov [howner], ax

    push ds
    push word clientclass
    push word 0
    push word 0
    push word WS_CHILD_VISIBLE_HI
    push word 0
    push word 0
    push word 0
    push word 400
    push word 300
    push word [howner]
    push word 1
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: its child, filling it
    mov [hclient], ax

    push ds
    push word ownedclass
    push ds
    push word ownedtitle
    push word (WS_OVERLAPPEDWINDOW >> 16) | WS_VISIBLE_HI
    push word WS_OVERLAPPEDWINDOW & 0xFFFF
    push word 150
    push word 80
    push word 220
    push word 140
    push word [hclient]             ; a child as the parent, as the Tutorial passes its client
    push word 0
    push word [hInstance]
    push word 0
    push word 0
    W16_CALL "USER", 41             ; CreateWindow: the owned window
    mov [howned], ax

    mov ax, [howner]
    mov si, key_owner
    call ini_hex
    mov ax, [hclient]
    mov si, key_client
    call ini_hex
    mov ax, [howned]
    mov si, key_owned
    call ini_hex

    push word [howner]
    push word 1
    push word 500
    push word 0
    push word 0
    W16_CALL "USER", 10             ; SetTimer(owner, 1, 500, NULL)

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

; AX as 4 hex digits under the key at SI: WritePrivateProfileString("Act", SI, text, "OWNACT.INI")
ini_hex:
    push si
    push ds
    pop es
    mov di, hex_t
    call hex16
    mov byte [di], 0
    pop si
    push ds
    push word ini_app
    push ds
    push si
    push ds
    push word hex_t
    push ds
    push word ini_file
    W16_CALL "KERNEL", 129
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

; The one procedure of all three classes, (hwnd, msg, wParam, lParam), Pascal, DS from AX.
wndproc:
    push bp
    mov bp, sp
    push ds
    push si
    push di
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_TIMER
    je .timer
    cmp ax, 0x0205                  ; WM_RBUTTONUP: SW_SHOW the owned window, 4 s from now
    jne .notright
    push word [howner]
    push word 2
    push word 4000
    push word 0
    push word 0
    W16_CALL "USER", 10             ; SetTimer(owner, 2, 4000, NULL)
    jmp .zero
.notright:
    cmp ax, WM_DESTROY
    jne .default
    mov ax, [bp+14]
    cmp ax, [howner]
    jne .default
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
    jmp .zero
.timer:
    cmp word [bp+10], 2
    jne .report
    push word [howner]
    push word 2
    W16_CALL "USER", 12             ; KillTimer(owner, 2): once
    push word [howned]
    push word 5
    W16_CALL "USER", 42             ; ShowWindow(owned, SW_SHOW): shown already, activated
    jmp .zero
.report:
    W16_CALL "USER", 60             ; GetActiveWindow
    mov si, key_active
    call ini_hex
    W16_CALL "USER", 23             ; GetFocus
    mov si, key_focus
    call ini_hex
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

W16_ENTRY start
W16_EXPORT 1, "WNDPROC", wndproc

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0

hInstance:  dw 0
howner:     dw 0
hclient:    dw 0
howned:     dw 0

wc:
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
wc_bg:      dw COLOR_WINDOW + 1
            dd 0
wc_name:    W16_FARPTR ownerclass, 2

msg:        times 18 db 0

ownerclass: db "OwnActOwner", 0
clientclass: db "OwnActClient", 0
ownedclass: db "OwnActOwned", 0
ownertitle: db "Owner", 0
ownedtitle: db "Owned", 0
ini_app:    db "Act", 0
ini_file:   db "OWNACT.INI", 0
key_owner:  db "Owner", 0
key_client: db "Client", 0
key_owned:  db "Owned", 0
key_active: db "Active", 0
key_focus:  db "Focus", 0
hex_t:      times 8 db 0
