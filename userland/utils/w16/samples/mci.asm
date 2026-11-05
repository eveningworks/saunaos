; mci.asm -- the Media Control Interface's AVIVideo device, as DK's encyclopaedias use it: SaunaOS's
; orbit.avi (Z:\USR\SHARE\VIDEOS\ORBIT.AVI, Motion JPEG, 150 frames at 15 a second) opened with an
; alias, drawn into the main window at (10,10) 120 by 68, played from frame 140 with notify; on
; MM_MCINOTIFY its destination moves to (160,10) and it seeks to frame 75, stopped -- only the seek's
; frame can be drawn there. tools/tests/gui/w16_gui_test.py's `mci` reads
; MCI.INI and the picture:
;
;   Open=<the device id>         Length=150 (frames, MCIAVI's first format)   LengthMs=10000
;   CmdLength=150 (mciSendCommand's MCI_STATUS)   Notify=1 (MCI_NOTIFY_SUCCESSFUL)   Mode=stopped
;   Position=75   Err=263 (MCIERR_INVALID_DEVICE_NAME, "play nosuch")   ErrText=<its text>
;   WhereWindow=<x> <y> 300 200 (the display window: the main one)
;   Open2=0 (mciSendCommand's MCI_OPEN, type "avivideo!<element>" and the element both given)
;   WinText=Clip (MCI_WINDOW's lpstrText, at its +12, become the own window's caption)
;   Notifies=1 (a "wait notify" play, from the first notification: one MM_MCINOTIFY, not two)
;
; Build: tools/gen/gen_ne.py.
;
;w16: module MCI
;w16: description MCI test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define WM_DESTROY     0x0002
%define WM_TIMER       0x0113
%define MM_MCINOTIFY   0x03B9
%define CW_USEDEFAULT  0x8000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define COLOR_WINDOW   5
%define IDC_ARROW      32512
%define MCI_STATUS     0x0814
%define MCI_STATUS_ITEM 0x0100
%define MCI_STATUS_LENGTH 1
%define MCI_OPEN       0x0803
%define MCI_WINDOW     0x0841
%define MCI_OPEN_ELEMENT 0x0200
%define MCI_OPEN_ALIAS 0x0400
%define MCI_OPEN_TYPE  0x2000
%define MCI_ANIM_WINDOW_TEXT 0x00080000

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
    push word mainclass
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
    push word [hwnd]
    push word 1
    push word 300
    push word 0
    push word 0
    W16_CALL "USER", 10             ; SetTimer(hwnd, 1, 300, NULL)

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

; mciSendString(SI, ret, 64, [hwnd]): AX the error
mci:
    push ds
    push si
    push ds
    push word ret
    push word 64
    push word [hwnd]
    W16_CALL "MMSYSTEM", 702
    ret

; WritePrivateProfileString("Mci", SI, DI, "MCI.INI")
ini:
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

; AX as decimal at DI, NUL-ended
dec:
    push ds
    pop es
    mov bx, 10
    xor cx, cx
.div:
    xor dx, dx
    div bx
    push dx
    inc cx
    or ax, ax
    jnz .div
.put:
    pop ax
    add al, '0'
    stosb
    loop .put
    mov byte [di], 0
    ret

; SI's string copied to DI (DI left at its NUL)
copy:
    push ds
    pop es
.c:
    lodsb
    stosb
    or al, al
    jnz .c
    dec di
    ret

; The one procedure, (hwnd, msg, wParam, lParam), Pascal, DS from AX.
wndproc:
    push bp
    mov bp, sp
    push ds
    push si
    push di
    mov ds, ax
    mov ax, [bp+12]
    cmp ax, WM_TIMER
    jne .nottimer
    cmp word [bp+10], 2
    je .count
    jmp .timer
.nottimer:
    cmp ax, MM_MCINOTIFY
    je .notified
    cmp ax, WM_DESTROY
    jne .default
    mov si, cmd_close
    call mci
    push word 0
    W16_CALL "USER", 6              ; PostQuitMessage
    jmp .zero

.timer:
    push word [hwnd]
    push word 1
    W16_CALL "USER", 12             ; KillTimer
    mov si, cmd_open
    call mci
    mov si, key_open
    mov di, ret
    call ini
    mov si, ret                     ; the device id, kept for mciSendCommand
    xor ax, ax
.atoi:
    mov bl, [si]
    inc si
    sub bl, '0'
    jb .gotid
    cmp bl, 9
    ja .gotid
    mov cx, 10
    mul cx
    xor bh, bh
    add ax, bx
    jmp .atoi
.gotid:
    mov [devid], ax
    mov si, cmd_window              ; "window movie handle <hwnd>"
    mov di, line
    call copy
    mov ax, [hwnd]
    call dec
    mov si, line
    call mci
    mov si, cmd_put
    call mci
    mov si, cmd_len
    call mci
    mov si, key_len
    mov di, ret
    call ini
    mov si, cmd_ms
    call mci
    mov si, cmd_len
    call mci
    mov si, key_lenms
    mov di, ret
    call ini
    mov si, cmd_frames
    call mci
    push word [devid]               ; mciSendCommand(id, MCI_STATUS, MCI_STATUS_ITEM, &st)
    push word MCI_STATUS
    push word 0
    push word MCI_STATUS_ITEM
    push ds
    push word st
    W16_CALL "MMSYSTEM", 701
    mov ax, [st_return]
    mov di, num
    call dec
    mov si, key_cmdlen
    mov di, num
    call ini
    mov si, cmd_nosuch              ; an error and its text
    call mci
    mov [err], ax
    mov di, num
    call dec
    mov si, key_err
    mov di, num
    call ini
    push word 0
    push word [err]
    push ds
    push word ret
    push word 128
    W16_CALL "MMSYSTEM", 706        ; mciGetErrorString
    mov si, key_errtext
    mov di, ret
    call ini
    mov si, cmd_play
    call mci
    jmp .zero

.notified:
    cmp word [phase], 0
    je .first
    inc word [notifies]             ; after the waited play: each one counted
    jmp .zero
.first:
    mov word [phase], 1
    mov ax, [bp+10]                 ; wParam: how the play ended
    mov di, num
    call dec
    mov si, key_notify
    mov di, num
    call ini
    mov si, cmd_mode
    call mci
    mov si, key_mode
    mov di, ret
    call ini
    mov si, cmd_where               ; the display window's place and size
    call mci
    mov si, key_where
    mov di, ret
    call ini
    mov si, cmd_waitplay            ; waited, with notify: its one MM_MCINOTIFY arrives as a message
    call mci
    push word [hwnd]
    push word 2
    push word 800
    push word 0
    push word 0
    W16_CALL "USER", 10             ; SetTimer(hwnd, 2, 800, NULL): then the count
    push word 0                     ; mciSendCommand(0, MCI_OPEN, TYPE|ELEMENT|ALIAS, &op)
    push word MCI_OPEN
    push word 0
    push word MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS
    push ds
    push word op
    W16_CALL "MMSYSTEM", 701
    mov di, num
    call dec
    mov si, key_open2
    mov di, num
    call ini
    push word [op_id]               ; mciSendCommand(two, MCI_WINDOW, MCI_ANIM_WINDOW_TEXT, &wp)
    push word MCI_WINDOW
    push word MCI_ANIM_WINDOW_TEXT >> 16
    push word 0
    push ds
    push word wnd
    W16_CALL "MMSYSTEM", 701
    mov si, cmd_wintext
    call mci
    mov si, key_wintext
    mov di, ret
    call ini
    mov si, cmd_close2
    call mci
    mov si, cmd_put2                ; a new place: only the seek's frame can land there
    call mci
    mov si, cmd_seek
    call mci
    mov si, cmd_pos
    call mci
    mov si, key_pos
    mov di, ret
    call ini
    jmp .zero

.count:
    push word [hwnd]
    push word 2
    W16_CALL "USER", 12             ; KillTimer
    mov ax, [notifies]
    mov di, num
    call dec
    mov si, key_notifies
    mov di, num
    call ini
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
nCmdShow:   dw 0
hwnd:       dw 0
devid:      dw 0
err:        dw 0
phase:      dw 0
notifies:   dw 0
op:         dd 0                    ; MCI_ANIM_OPEN_PARMS: dwCallback, wDeviceID, wReserved0,
op_id:      dw 0, 0                 ; lpstrDeviceType, lpstrElementName, lpstrAlias, dwStyle,
            W16_FARPTR type_bang, 2 ; hWndParent, wReserved1
            W16_FARPTR elem_two, 2
            W16_FARPTR alias_two, 2
            dd 0
            dw 0, 0
wnd:        dd 0                    ; MCI_ANIM_WINDOW_PARMS: dwCallback, hWnd, wReserved1, nCmdShow,
            dw 0, 0, 0, 0           ; wReserved2, lpstrText
            W16_FARPTR text_clip, 2
st:         dd 0                    ; MCI_STATUS_PARMS: dwCallback, dwReturn, dwItem, dwTrack
st_return:  dd 0
            dd MCI_STATUS_LENGTH
            dd 0

wc:
            dw 0
            W16_FARPTR wndproc, 1
            dw 0, 0
wc_hInstance: dw 0
            dw 0
wc_hCursor: dw 0
            dw COLOR_WINDOW + 1
            dd 0
            W16_FARPTR mainclass, 2

msg:
            dw 0, 0
msg_wParam: dw 0
            dd 0, 0
            dw 0, 0

mainclass:  db "MciMain", 0
title:      db "MCI test", 0
ini_app:    db "Mci", 0
ini_file:   db "MCI.INI", 0
cmd_open:   db "open Z:\USR\SHARE\VIDEOS\ORBIT.AVI alias movie", 0
cmd_window: db "window movie handle ", 0
cmd_put:    db "put movie destination at 10 10 120 68", 0
cmd_len:    db "status movie length", 0
cmd_ms:     db "set movie time format ms", 0
cmd_frames: db "set movie time format frames", 0
cmd_nosuch: db "play nosuch", 0
cmd_play:   db "play movie from 140 notify", 0
cmd_mode:   db "status movie mode", 0
cmd_put2:   db "put movie destination at 160 10 120 68", 0
cmd_seek:   db "seek movie to 75", 0
cmd_pos:    db "status movie position", 0
cmd_close:  db "close movie", 0
cmd_where:  db "where movie window", 0
cmd_waitplay: db "play movie from 145 to 148 wait notify", 0
cmd_wintext: db "info two window text", 0
cmd_close2: db "close two", 0
type_bang:  db "avivideo!Z:\USR\SHARE\VIDEOS\ORBIT.AVI", 0
elem_two:   db "Z:\USR\SHARE\VIDEOS\ORBIT.AVI", 0
alias_two:  db "two", 0
text_clip:  db "Clip", 0
key_where:  db "WhereWindow", 0
key_open2:  db "Open2", 0
key_wintext: db "WinText", 0
key_notifies: db "Notifies", 0
key_open:   db "Open", 0
key_len:    db "Length", 0
key_lenms:  db "LengthMs", 0
key_cmdlen: db "CmdLength", 0
key_err:    db "Err", 0
key_errtext: db "ErrText", 0
key_notify: db "Notify", 0
key_mode:   db "Mode", 0
key_pos:    db "Position", 0
num:        times 8 db 0
line:       times 64 db 0
ret:        times 130 db 0
