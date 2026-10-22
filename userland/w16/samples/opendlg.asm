; opendlg.asm -- GetOpenFileName on a template of the program's own (OFN_ENABLETEMPLATE, DIALOG 100,
; COMMDLG's standard ids) with a hook. On WM_INITDIALOG the hook records which of Read Only (chx1)
; and Help (psh15) are shown, fills in a name, ticks or unticks Read Only and presses OK or Cancel.
; What came back goes to OPENDLG.INI's [OpenDlg] section, numbers as hex:
;   Vis1=0000 Ret1=0000            OFN_HIDEREADONLY, no OFN_SHOWHELP: neither shown; Cancel
;   Vis2=0003 Ret2=0001 Ro2=0001   OFN_SHOWHELP: both shown (bit 0 Read Only, bit 1 Help); OK with
;                                  Read Only ticked: OFN_READONLY comes back set
;   Ret3=0001 Ro3=0000             OFN_READONLY going in, the box unticked: it comes back clear
; Build: tools/gen_ne.py.
;
;w16: module OPENDLG
;w16: description Open dialog test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024
;w16: dialog 100 36 24 264 134 0x80c80080 "Open Test"
;w16: control 100 STATIC 1090 6 6 76 9 0x50000000 "File &Name:"
;w16: control 100 EDIT 1152 6 16 90 12 0x50810080 ""
;w16: control 100 LISTBOX 1120 6 32 90 68 0x50a10053 ""
;w16: control 100 STATIC 65535 110 6 92 9 0x50000000 "&Directories:"
;w16: control 100 STATIC 1088 110 18 92 9 0x50000080 ""
;w16: control 100 LISTBOX 1121 110 32 92 68 0x50a10053 ""
;w16: control 100 STATIC 1089 6 104 90 9 0x50000000 "List Files of &Type:"
;w16: control 100 COMBOBOX 1136 6 114 90 36 0x50a10013 ""
;w16: control 100 STATIC 1091 110 104 92 9 0x50000000 "Dri&ves:"
;w16: control 100 COMBOBOX 1137 110 114 92 68 0x50a10053 ""
;w16: control 100 BUTTON 1 208 6 50 14 0x50010001 "OK"
;w16: control 100 BUTTON 2 208 24 50 14 0x50010000 "Cancel"
;w16: control 100 BUTTON 1038 208 46 50 14 0x50010000 "&Help"
;w16: control 100 BUTTON 1040 208 68 50 12 0x50010003 "&Read Only"

%include "ne16.inc"

%define WM_INITDIALOG    0x0110
%define WM_COMMAND       0x0111
%define OFN_READONLY     0x0001
%define OFN_HIDEREADONLY 0x0004
%define OFN_SHOWHELP     0x0010
%define OFN_ENABLEHOOK   0x0020
%define OFN_ENABLETEMPLATE 0x0040

; HEXW buffer, word -- four hex digits and a NUL at ES:buffer.
%macro HEXW 2
    mov di, %1
    mov ax, %2
    call hex16
    mov byte [di], 0
%endmacro

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz fail
    mov [ofn + 6], di               ; hInstance: the template is the program's own

    mov word [mode], 1              ; Read Only hidden, no Help; Cancel
    mov word [ofn + 48], OFN_ENABLETEMPLATE | OFN_ENABLEHOOK | OFN_HIDEREADONLY
    call open
    mov [ret1], ax
    mov ax, [vis]
    mov [vis1], ax

    mov word [mode], 2              ; both shown; OK with Read Only ticked
    mov word [ofn + 48], OFN_ENABLETEMPLATE | OFN_ENABLEHOOK | OFN_SHOWHELP
    call open
    mov [ret2], ax
    mov ax, [vis]
    mov [vis2], ax
    mov ax, [ofn + 48]
    and ax, OFN_READONLY
    mov [ro2], ax

    mov word [mode], 3              ; ticked going in; OK with it unticked
    mov word [ofn + 48], OFN_ENABLETEMPLATE | OFN_ENABLEHOOK | OFN_SHOWHELP | OFN_READONLY
    call open
    mov [ret3], ax
    mov ax, [ofn + 48]
    and ax, OFN_READONLY
    mov [ro3], ax

    push ds                         ; the numbers, as text
    pop es
    HEXW vis1_t, [vis1]
    HEXW ret1_t, [ret1]
    HEXW vis2_t, [vis2]
    HEXW ret2_t, [ret2]
    HEXW ro2_t, [ro2]
    HEXW ret3_t, [ret3]
    HEXW ro3_t, [ro3]

    mov si, keys
.write:
    lodsw                           ; a key's name, then its text
    or ax, ax
    jz .written
    mov dx, ax
    lodsw
    push si
    push ds                         ; WritePrivateProfileString("OpenDlg", key, text, "OPENDLG.INI")
    push word ini_app
    push ds
    push dx
    push ds
    push ax
    push ds
    push word ini_file
    W16_CALL "KERNEL", 129
    pop si
    jmp .write
.written:
    mov ax, 0x4C00
    int 0x21
fail:
    mov ax, 0x4C01
    int 0x21

; GetOpenFileName(&ofn), an empty name going in: its answer in AX.
open:
    mov byte [file], 0
    mov word [vis], 0xFFFF
    push ds
    push word ofn
    W16_CALL "COMMDLG", 1
    ret

; Whether control `id` of the dialog is WS_VISIBLE: 1 or 0 in AX. BX: the dialog.
visible:
    push bx
    push ax
    W16_CALL "USER", 91             ; GetDlgItem(hDlg, id)
    push ax
    push word -16
    W16_CALL "USER", 135            ; GetWindowLong(hwnd, GWL_STYLE)
    mov ax, dx
    shr ax, 12
    and ax, 1                       ; WS_VISIBLE, 0x10000000
    ret

; The hook: (hDlg, msg, wParam, lParam), Pascal, DS from AX.
hook:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    cmp word [bp+12], WM_INITDIALOG
    jne .no
    mov bx, [bp+14]
    mov ax, 1040                    ; chx1: bit 0
    call visible
    mov [vis], ax
    mov bx, [bp+14]
    mov ax, 1038                    ; psh15: bit 1
    call visible
    shl ax, 1
    or [vis], ax
    mov cx, 2                       ; mode 1: Cancel
    cmp word [mode], 1
    je .press
    push word [bp+14]               ; SetDlgItemText(hDlg, edt1, "TEST.TXT")
    push word 1152
    push ds
    push word name
    W16_CALL "USER", 92
    xor ax, ax                      ; CheckDlgButton(hDlg, chx1, mode 2 ? 1 : 0)
    cmp word [mode], 2
    jne .check
    inc ax
.check:
    push word [bp+14]
    push word 1040
    push ax
    W16_CALL "USER", 97
    mov cx, 1                       ; OK
.press:
    push word [bp+14]               ; PostMessage(hDlg, WM_COMMAND, id, 0)
    push word WM_COMMAND
    push cx
    push word 0
    push word 0
    W16_CALL "USER", 110
.no:
    xor ax, ax
    xor dx, dx
    pop ds
    pop bp
    retf 10

; AX as four hex digits at ES:DI.
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

W16_ENTRY start
W16_EXPORT 1, "HOOK", hook

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            times 16 db 0           ; the Windows header words a task keeps
ofn:        dd 72                   ; lStructSize
            dw 0, 0                 ; hwndOwner, hInstance
            W16_FARPTR filter, 2    ; lpstrFilter
            dd 0, 0                 ; lpstrCustomFilter, nMaxCustFilter
            dd 1                    ; nFilterIndex
            W16_FARPTR file, 2      ; lpstrFile
            dd 128                  ; nMaxFile
            dd 0, 0, 0, 0           ; lpstrFileTitle, nMaxFileTitle, lpstrInitialDir, lpstrTitle
            dd 0                    ; Flags
            dw 0, 0                 ; nFileOffset, nFileExtension
            dd 0, 0                 ; lpstrDefExt, lCustData
            W16_FARPTR hook, 1      ; lpfnHook
            dd 100                  ; lpTemplateName: MAKEINTRESOURCE(100)
filter:     db "All Files (*.*)", 0, "*.*", 0, 0
file:       times 128 db 0
name:       db "TEST.TXT", 0
mode:       dw 0
vis:        dw 0
vis1:       dw 0
vis2:       dw 0
ret1:       dw 0
ret2:       dw 0
ret3:       dw 0
ro2:        dw 0
ro3:        dw 0
vis1_t:     times 6 db 0
ret1_t:     times 6 db 0
vis2_t:     times 6 db 0
ret2_t:     times 6 db 0
ro2_t:      times 6 db 0
ret3_t:     times 6 db 0
ro3_t:      times 6 db 0
keys:       dw k_vis1, vis1_t, k_ret1, ret1_t, k_vis2, vis2_t, k_ret2, ret2_t, k_ro2, ro2_t
            dw k_ret3, ret3_t, k_ro3, ro3_t, 0
k_vis1:     db "Vis1", 0
k_ret1:     db "Ret1", 0
k_vis2:     db "Vis2", 0
k_ret2:     db "Ret2", 0
k_ro2:      db "Ro2", 0
k_ret3:     db "Ret3", 0
k_ro3:      db "Ro3", 0
ini_app:    db "OpenDlg", 0
ini_file:   db "OPENDLG.INI", 0
