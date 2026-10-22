; wincalls.asm -- calls that need no window, answered, to WINCALLS.INI (drive C:'s
; WINDOWS folder), one [WinCalls] key each, numbers as hex:
;   EqualSame=0001 EqualDiff=0000      EqualRect on two rectangles
;   Upper=ABC XYZ1 UpperN=0007         AnsiUpperBuff
;   Wvs=42|toy|beef|100000 WvsN=0012   wvsprintf: %d %s %x %lu
;   WinDir=C:\WINDOWS SysDir=C:\WINDOWS\SYSTEM
;   Lru=0001                           GlobalLRUNewest answers its handle
;   Bk=00123456 Text=00654321          SetBkColor / SetTextColor, read back
;   BkOld=00FFFFFF TextOld=00000000    ...and what they returned
;   Align=0006 AlignOld=0000           SetTextAlign(6), GetTextAlign
;   Pal=0001                           CreatePalette answered a handle
;   HugeSize=00030000 HugeTile=005A    a 192 KB GlobalAlloc: its size, a byte in its third 64 KB
;   HugeFree=0000                      ...and GlobalFree
;   Free=00100000 Temp=3A43            GetFreeSpace, GetTempDrive
;   Drv0=0000 Drv2=0003                GetDriveType: A: none, C: fixed
;   Int2f1686=0000 Int2f1600=0A03      INT 2Fh: protected mode, and the Windows version
;   Rop2=000D Org=00000000             GetROP2 of a new DC, GetBrushOrg
;   BitsSet=00000008 BitsGet=00000008  Set- and GetBitmapBits of a 4 x 2 bitmap
;   BitsOut=00F9FF04F9FF0004           ...the bytes back, indices of the display's static colours
;   DibW=0004 DibH=0002                GetDIBits fills the header in
;   Dib24=...                          ...and 24-bit rows, bottom row first, in BGR
; Run on real Windows 3.11 it is the check of what each answer should be.
; Build: tools/gen_ne.py.
;
;w16: module WINCALLS
;w16: description Windows calls test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

; HEXW buffer, word -- four hex digits and a NUL at ES:buffer.
%macro HEXW 2
    mov di, %1
    mov ax, %2
    call hex16
    mov byte [di], 0
%endmacro

; HEXD buffer, dword -- eight hex digits and a NUL.
%macro HEXD 2
    mov di, %1
    mov ax, [%2 + 2]
    call hex16
    mov ax, [%2]
    call hex16
    mov byte [di], 0
%endmacro

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz fail

    push ds                         ; EqualRect(&a, &a)
    push word rect_a
    push ds
    push word rect_a
    W16_CALL "USER", 244
    mov [equal_same], ax
    push ds                         ; EqualRect(&a, &b)
    push word rect_a
    push ds
    push word rect_b
    W16_CALL "USER", 244
    mov [equal_diff], ax

    push ds                         ; AnsiUpperBuff(buf, 7): "abc xyz1" -> "ABC XYZ1"
    push word upper
    push word 7
    W16_CALL "USER", 437
    mov [upper_n], ax

    mov word [args + 0], 42         ; wvsprintf(out, "%d|%s|%x|%lu", args)
    mov word [args + 2], str_toy
    mov [args + 4], ds
    mov word [args + 6], 0xBEEF
    mov word [args + 8], 0x86A0     ; 100000
    mov word [args + 10], 0x0001
    push ds
    push word wvs
    push ds
    push word wvs_fmt
    push ds
    push word args
    W16_CALL "USER", 421
    mov [wvs_n], ax

    push ds                         ; GetWindowsDirectory(buf, 64)
    push word windir
    push word 64
    W16_CALL "KERNEL", 134
    push ds                         ; GetSystemDirectory(buf, 64)
    push word sysdir
    push word 64
    W16_CALL "KERNEL", 135

    push word 2                     ; GlobalAlloc(GMEM_MOVEABLE, 64)
    push word 0
    push word 64
    W16_CALL "KERNEL", 15
    mov [hmem], ax
    push ax                         ; GlobalLRUNewest(h) answers h
    W16_CALL "KERNEL", 164
    xor cx, cx
    cmp ax, [hmem]
    jne .lru
    inc cx
.lru:
    mov [lru], cx

    push word 0                     ; CreateCompatibleDC(0)
    W16_CALL "GDI", 52
    mov [hdc], ax
    push ax                         ; SetBkColor(hdc, 0x00123456): the old colour
    push word 0x0012
    push word 0x3456
    W16_CALL "GDI", 1
    mov [bk_old], ax
    mov [bk_old + 2], dx
    push word [hdc]                 ; GetBkColor(hdc)
    W16_CALL "GDI", 75
    mov [bk], ax
    mov [bk + 2], dx
    push word [hdc]                 ; SetTextColor(hdc, 0x00654321)
    push word 0x0065
    push word 0x4321
    W16_CALL "GDI", 9
    mov [tcol_old], ax
    mov [tcol_old + 2], dx
    push word [hdc]                 ; GetTextColor(hdc)
    W16_CALL "GDI", 90
    mov [tcol], ax
    mov [tcol + 2], dx
    push word [hdc]                 ; SetTextAlign(hdc, 6): the old word
    push word 6
    W16_CALL "GDI", 346
    mov [t_align_old], ax
    push word [hdc]                 ; GetTextAlign(hdc)
    W16_CALL "GDI", 345
    mov [t_align], ax

    push ds                         ; CreatePalette(&logpal): a handle, or 0
    push word logpal
    W16_CALL "GDI", 360
    xor cx, cx
    or ax, ax
    jz .pal
    inc cx
.pal:
    mov [pal], cx

    push word 2                     ; GlobalAlloc(GMEM_MOVEABLE, 0x30000): a huge block, three selectors
    push word 3
    push word 0
    W16_CALL "KERNEL", 15
    mov [hhuge], ax
    push ax                         ; GlobalSize(h)
    W16_CALL "KERNEL", 20
    mov [huge_size], ax
    mov [huge_size + 2], dx
    push word [hhuge]               ; GlobalLock(h): sel:0
    W16_CALL "KERNEL", 18
    add dx, 16                      ; two tiles on, __AHINCR (8) a tile
    mov es, dx
    mov byte [es:0], 0x5A
    mov al, [es:0]
    xor ah, ah
    mov [huge_tile], ax
    push word [hhuge]               ; GlobalFree(h): 0
    W16_CALL "KERNEL", 17
    mov [huge_free], ax

    push word 0                     ; GetFreeSpace(0)
    W16_CALL "KERNEL", 169
    mov [free_sp], ax
    mov [free_sp + 2], dx
    push word 'C'                   ; GetTempDrive('C')
    W16_CALL "KERNEL", 92
    mov [temp_dr], ax
    push word 0                     ; GetDriveType(0): A:
    W16_CALL "KERNEL", 136
    mov [drv0], ax
    push word 2                     ; GetDriveType(2): C:
    W16_CALL "KERNEL", 136
    mov [drv2], ax

    mov ax, 0x1686                  ; INT 2Fh: real or V86 mode?
    int 0x2F
    mov [i2f86], ax
    mov ax, 0x1600                  ; INT 2Fh: Windows enhanced mode, the version
    int 0x2F
    mov [i2f00], ax

    push word [hdc]                 ; GetROP2(hdc)
    W16_CALL "GDI", 85
    mov [rop2], ax
    push word [hdc]                 ; GetBrushOrg(hdc)
    W16_CALL "GDI", 149
    mov [borg], ax
    mov [borg + 2], dx

    push word 4                     ; CreateBitmap(4, 2, 1, 8, NULL)
    push word 2
    push word 1
    push word 8
    push word 0
    push word 0
    W16_CALL "GDI", 48
    mov [hbm], ax
    push ax                         ; SetBitmapBits(hbm, 8, bits_in)
    push word 0
    push word 8
    push ds
    push word bits_in
    W16_CALL "GDI", 106
    mov [bits_set], ax
    mov [bits_set + 2], dx
    push word [hbm]                 ; GetBitmapBits(hbm, 8, bits_out)
    push word 0
    push word 8
    push ds
    push word bits_out
    W16_CALL "GDI", 74
    mov [bits_get], ax
    mov [bits_get + 2], dx
    push word [hdc]                 ; GetDIBits(hdc, hbm, 0, 2, dib, bmi, DIB_RGB_COLORS), 24 bits
    push word [hbm]
    push word 0
    push word 2
    push ds
    push word dib
    push ds
    push word bmi
    push word 0
    W16_CALL "GDI", 441
    mov ax, [bmi + 4]
    mov [dib_w], ax
    mov ax, [bmi + 8]
    mov [dib_h], ax

    push ds                         ; the numbers, as text
    pop es
    HEXW equal_same_t, [equal_same]
    HEXW equal_diff_t, [equal_diff]
    HEXW upper_n_t, [upper_n]
    HEXW wvs_n_t, [wvs_n]
    HEXW lru_t, [lru]
    HEXW align_t, [t_align]
    HEXW align_old_t, [t_align_old]
    HEXW pal_t, [pal]
    HEXD bk_t, bk
    HEXD bk_old_t, bk_old
    HEXD text_t, tcol
    HEXD text_old_t, tcol_old
    HEXD huge_size_t, huge_size
    HEXW huge_tile_t, [huge_tile]
    HEXW huge_free_t, [huge_free]
    HEXD free_t, free_sp
    HEXW temp_t, [temp_dr]
    HEXW drv0_t, [drv0]
    HEXW drv2_t, [drv2]
    HEXW i2f86_t, [i2f86]
    HEXW i2f00_t, [i2f00]
    HEXW rop2_t, [rop2]
    HEXD org_t, borg
    HEXD bits_set_t, bits_set
    HEXD bits_get_t, bits_get
    HEXW dib_w_t, [dib_w]
    HEXW dib_h_t, [dib_h]
    mov di, bits_out_t              ; the eight bytes read back, as hex
    mov si, bits_out
    mov cx, 8
.bo:
    lodsb
    xor ah, ah
    push cx
    push si
    call hex8
    pop si
    pop cx
    loop .bo
    mov byte [di], 0
    mov di, dib24_t                 ; the first 24 bytes of the DIB, as hex
    mov si, dib
    mov cx, 24
.dd:
    lodsb
    push cx
    push si
    call hex8
    pop si
    pop cx
    loop .dd
    mov byte [di], 0

    mov si, keys
.write:
    lodsw                           ; a key's name, then its text
    or ax, ax
    jz .written
    mov dx, ax
    lodsw
    push si                         ; kept across the call, under its arguments
    push ds                         ; WritePrivateProfileString("WinCalls", key, text, "WINCALLS.INI")
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

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            times 16 db 0           ; the Windows header words a task keeps
rect_a:     dw 1, 2, 30, 40
rect_b:     dw 1, 2, 30, 41
upper:      db "abc xyz1", 0
wvs_fmt:    db "%d|%s|%x|%lu", 0
str_toy:    db "toy", 0
logpal:     dw 0x0300, 2            ; version, two entries
            db 255, 0, 0, 0, 0, 0, 255, 0
args:       times 12 db 0
wvs:        times 40 db 0
windir:     times 64 db 0
sysdir:     times 64 db 0
equal_same: dw 0
equal_diff: dw 0
upper_n:    dw 0
wvs_n:      dw 0
hmem:       dw 0
lru:        dw 0
hdc:        dw 0
bk:         dd 0
bk_old:     dd 0
tcol:       dd 0
tcol_old:   dd 0
t_align:    dw 0
t_align_old: dw 0
pal:        dw 0
hhuge:      dw 0
huge_size:  dd 0
huge_tile:  dw 0
huge_free:  dw 0
free_sp:    dd 0
temp_dr:    dw 0
drv0:       dw 0
drv2:       dw 0
i2f86:      dw 0
i2f00:      dw 0
rop2:       dw 0
borg:       dd 0
hbm:        dw 0
bits_set:   dd 0
bits_get:   dd 0
dib_w:      dw 0
dib_h:      dw 0
bits_in:    db 0x00, 0xF9, 0xFF, 0x04, 0xF9, 0xFF, 0x00, 0x04
bits_out:   times 8 db 0
bmi:        dd 40, 0, 0
            dw 0, 24
            times 24 db 0
dib:        times 32 db 0

equal_same_t: times 6 db 0
equal_diff_t: times 6 db 0
upper_n_t:  times 6 db 0
wvs_n_t:    times 6 db 0
lru_t:      times 6 db 0
align_t:    times 6 db 0
align_old_t: times 6 db 0
pal_t:      times 6 db 0
bk_t:       times 10 db 0
bk_old_t:   times 10 db 0
text_t:     times 10 db 0
text_old_t: times 10 db 0
huge_size_t: times 10 db 0
huge_tile_t: times 6 db 0
huge_free_t: times 6 db 0
free_t:     times 10 db 0
temp_t:     times 6 db 0
drv0_t:     times 6 db 0
drv2_t:     times 6 db 0
i2f86_t:    times 6 db 0
i2f00_t:    times 6 db 0
rop2_t:     times 6 db 0
org_t:      times 10 db 0
bits_set_t: times 10 db 0
bits_get_t: times 10 db 0
dib_w_t:    times 6 db 0
dib_h_t:    times 6 db 0
bits_out_t: times 20 db 0
dib24_t:    times 52 db 0

keys:       dw k_equal_same, equal_same_t, k_equal_diff, equal_diff_t
            dw k_upper, upper, k_upper_n, upper_n_t
            dw k_wvs, wvs, k_wvs_n, wvs_n_t
            dw k_windir, windir, k_sysdir, sysdir
            dw k_lru, lru_t, k_bk, bk_t, k_bk_old, bk_old_t
            dw k_text, text_t, k_text_old, text_old_t
            dw k_align, align_t, k_align_old, align_old_t
            dw k_huge_size, huge_size_t, k_huge_tile, huge_tile_t, k_huge_free, huge_free_t
            dw k_free, free_t, k_temp, temp_t, k_drv0, drv0_t, k_drv2, drv2_t
            dw k_i2f86, i2f86_t, k_i2f00, i2f00_t, k_rop2, rop2_t, k_org, org_t
            dw k_bits_set, bits_set_t, k_bits_get, bits_get_t, k_bits_out, bits_out_t
            dw k_dib_w, dib_w_t, k_dib_h, dib_h_t, k_dib24, dib24_t
            dw k_pal, pal_t, 0
k_equal_same: db "EqualSame", 0
k_equal_diff: db "EqualDiff", 0
k_upper:    db "Upper", 0
k_upper_n:  db "UpperN", 0
k_wvs:      db "Wvs", 0
k_wvs_n:    db "WvsN", 0
k_windir:   db "WinDir", 0
k_sysdir:   db "SysDir", 0
k_lru:      db "Lru", 0
k_bk:       db "Bk", 0
k_bk_old:   db "BkOld", 0
k_text:     db "Text", 0
k_text_old: db "TextOld", 0
k_align:    db "Align", 0
k_align_old: db "AlignOld", 0
k_pal:      db "Pal", 0
k_huge_size: db "HugeSize", 0
k_huge_tile: db "HugeTile", 0
k_huge_free: db "HugeFree", 0
k_free:     db "Free", 0
k_temp:     db "Temp", 0
k_drv0:     db "Drv0", 0
k_drv2:     db "Drv2", 0
k_i2f86:    db "Int2f1686", 0
k_i2f00:    db "Int2f1600", 0
k_rop2:     db "Rop2", 0
k_org:      db "Org", 0
k_bits_set: db "BitsSet", 0
k_bits_get: db "BitsGet", 0
k_bits_out: db "BitsOut", 0
k_dib_w:    db "DibW", 0
k_dib_h:    db "DibH", 0
k_dib24:    db "Dib24", 0
ini_app:    db "WinCalls", 0
ini_file:   db "WINCALLS.INI", 0
