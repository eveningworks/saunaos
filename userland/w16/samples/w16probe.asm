; w16probe.asm -- answers Windows gives that are easy to get subtly wrong, written to W16PROBE.INI in
; drive C:'s WINDOWS folder:
;   [Probe] Base=   GetDialogBaseUnits (DX:AX: the System font's height and width)
;           Async=  GetAsyncKeyState(VK_LBUTTON), no button down
;           Wide1000= / Wide100=  GetTextExtent("00:00:00") in Arial at 1000 and 100 pixels -- Clock sizes
;                   its digits by measuring them in a 1000-pixel font, so the first must be ten times the
;                   second, not cut at what the rasterizer can draw
;           Not=    GetPixel of light grey after an R2_NOT line: dark grey (00808080) on 3.1's palette, where
;                   the mix works on palette indices -- Clock's second hand
;           Map0= / Map1= / Map2=  MapVirtualKey: VK_A to its scan code (001E), that back to VK_A (0041),
;                   VK_OEM_1 to the character it types (003B, ';')
;           Families= / TrueType=  EnumFontFamilies with no family named: how many families, and how
;                   many of them TrueType -- Character Map's Font list
;           Dash=   GetTextExtent of 0x97, the em dash in 3.1's ANSI table, in Arial at 100 pixels: an em,
;                   about 0064 -- not a space's width, which a byte with no glyph measured
;           Border= / Border2=  SystemParametersInfo: SPI_GETBORDER (3.1's 3 with no WIN.INI key), then
;                   again after SPI_SETBORDER 5 for the session (0005) -- Control Panel's Desktop
;           Title=  SPI_GETICONTITLELOGFONT's height: MS Sans Serif 8 point at 96 dpi, FFF5 (-11)
;           Ver= / VerDest=  VerFindFile of a shared file no disk has: 0, and C:\WINDOWS\SYSTEM's length
;                   with its NUL (0012) -- Control Panel's applets ask it at start
;           Vis=    RectVisible on an 8x1 bitmap: a rectangle on it (0001), then one far off it (0000)
;           Dbl=    GetDoubleClickTime after SetDoubleClickTime(300): 012C
;           Swap=   SwapMouseButton(TRUE) then (FALSE): what each says the buttons were -- 00000001
;           LZ= / LZRead= / LZEnd= / LZText= / LZName=  LZEXPAND on a 10-byte SZDD file the probe writes to
;                   C:\WINDOWS and opens by its bare name -- OpenFile's search, as Control Panel opens
;                   CONTROL.INF: an LZ handle (0190, the first), the count read (000A), LZSeek to the end
;                   (0000000A), the expanded text, and GetExpandedName's LZPROBE.TXT
; Run on real Windows 3.11 for Workgroups and on w16, it is the check of both: w16_gui_test.py reads
; the file. No window. Its name sorts after fault.exe: tests index the samples by their place.
; Build: tools/gen_ne.py.
;
;w16: module W16PROBE
;w16: description Probe test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jnz .run
    jmp fail
.run:
    mov [hinst], di
    push ds
    pop es

    W16_CALL "USER", 243            ; GetDialogBaseUnits: DX height, AX width
    push ax
    mov ax, dx
    mov di, base_text
    call hex16
    pop ax
    call hex16
    mov byte [di], 0

    push word 1                     ; GetAsyncKeyState(VK_LBUTTON)
    W16_CALL "USER", 249
    mov di, async_text
    call hex16
    mov byte [di], 0

    push word 0                     ; GetDC(NULL): the screen
    W16_CALL "USER", 66
    mov [screen], ax
    push ax                         ; CreateCompatibleDC(screen)
    W16_CALL "GDI", 52
    mov [mem], ax

    mov word [logfont], -1000       ; the text extent at 1000 pixels, then at 100
    mov di, wide1000_text
    call extent
    mov word [logfont], -100
    mov di, wide100_text
    call extent
    mov word [ext_str], dash        ; the em dash alone, at the same 100 pixels
    mov word [ext_len], 1
    mov di, dash_text
    call extent

    push word [screen]              ; CreateCompatibleBitmap(screen, 8, 1): colour, as the screen is
    push word 8
    push word 1
    W16_CALL "GDI", 51
    push word [mem]
    push ax
    W16_CALL "GDI", 45              ; SelectObject(mem, bitmap)
    push word 0x00C0                ; CreateSolidBrush(0x00C0C0C0)
    push word 0xC0C0
    W16_CALL "GDI", 66
    push word [mem]
    push ax
    W16_CALL "GDI", 45              ; SelectObject(mem, brush)
    push word [mem]                 ; PatBlt(mem, 0, 0, 8, 1, PATCOPY): light grey
    push word 0
    push word 0
    push word 8
    push word 1
    push word 0x00F0
    push word 0x0021
    W16_CALL "GDI", 29
    push word [mem]                 ; SetROP2(mem, R2_NOT)
    push word 6
    W16_CALL "GDI", 4
    push word [mem]                 ; MoveTo(mem, 0, 0)
    push word 0
    push word 0
    W16_CALL "GDI", 20
    push word [mem]                 ; LineTo(mem, 8, 0)
    push word 8
    push word 0
    W16_CALL "GDI", 19
    push word [mem]                 ; GetPixel(mem, 2, 0): DX:AX a COLORREF
    push word 2
    push word 0
    W16_CALL "GDI", 83
    push ax
    mov ax, dx
    mov di, not_text
    call hex16
    pop ax
    call hex16
    mov byte [di], 0

    mov si, maps                    ; MapVirtualKey(code, type) for each row of `maps`
.map:
    lodsw
    or ax, ax
    jz .mapped
    mov cx, ax                      ; the code
    lodsw
    mov dx, ax                      ; the map type
    lodsw
    push si                         ; kept across the call, under its arguments
    push ax                         ; where its answer goes
    push cx
    push dx
    W16_CALL "KEYBOARD", 131
    pop di
    call hex16
    mov byte [di], 0
    pop si
    jmp .map
.mapped:

    push word 5                     ; SystemParametersInfo(SPI_GETBORDER, 0, &value, 0)
    push word 0
    push ds
    push word value
    push word 0
    W16_CALL "USER", 483
    mov ax, [value]
    mov di, border_text
    call hex16
    mov byte [di], 0
    push word 6                     ; SPI_SETBORDER 5, for this run only
    push word 5
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 483
    push word 5                     ; SPI_GETBORDER again
    push word 0
    push ds
    push word value
    push word 0
    W16_CALL "USER", 483
    mov ax, [value]
    mov di, border2_text
    call hex16
    mov byte [di], 0
    push word 31                    ; SPI_GETICONTITLELOGFONT(sizeof LOGFONT, &title_lf)
    push word 50
    push ds
    push word title_lf
    push word 0
    W16_CALL "USER", 483
    mov ax, [title_lf]
    mov di, title_text
    call hex16
    mov byte [di], 0

    push word 1                     ; VerFindFile(VFFF_ISSHAREDFILE, "NOSUCH.DLL", NULL, NULL,
    push ds                         ;   cur, &cur_len, dest, &dest_len)
    push word nosuch
    push word 0
    push word 0
    push word 0
    push word 0
    push ds
    push word ver_cur
    push ds
    push word cur_len
    push ds
    push word ver_dest
    push ds
    push word dest_len
    W16_CALL "VER", 8
    mov di, ver_text
    call hex16
    mov byte [di], 0
    mov ax, [dest_len]
    mov di, verdest_text
    call hex16
    mov byte [di], 0

    push word [mem]                 ; RectVisible(mem, on): the 8x1 bitmap is still selected
    push ds
    push word rect_on
    W16_CALL "GDI", 465
    mov di, vis_text
    call hex16
    push di
    push word [mem]                 ; RectVisible(mem, off)
    push ds
    push word rect_off
    W16_CALL "GDI", 465
    pop di
    call hex16
    mov byte [di], 0

    push ds                         ; _lcreat("C:\WINDOWS\LZPROBE.TX_", 0), and the SZDD bytes written
    push word lz_path
    push word 0
    W16_CALL "KERNEL", 83
    push ax
    push ax
    push ds
    push word szdd
    push word szdd_len
    W16_CALL "KERNEL", 86           ; _lwrite
    W16_CALL "KERNEL", 81           ; _lclose (the handle pushed first)
    push ds                         ; LZOpenFile("LZPROBE.TX_", &lz_ofs, OF_READ)
    push word lz_bare
    push ds
    push word lz_ofs
    push word 0
    W16_CALL "LZEXPAND", 2
    mov [lzh], ax
    mov di, lz_text
    call hex16
    mov byte [di], 0
    push word [lzh]                 ; LZRead(h, lz_buf, 32)
    push ds
    push word lz_buf
    push word 32
    W16_CALL "LZEXPAND", 5
    mov di, lzread_text
    call hex16
    mov byte [di], 0
    push word [lzh]                 ; LZSeek(h, 0, 2): DX:AX the length
    push word 0
    push word 0
    push word 2
    W16_CALL "LZEXPAND", 4
    push ax
    mov ax, dx
    mov di, lzend_text
    call hex16
    pop ax
    call hex16
    mov byte [di], 0
    push word [lzh]                 ; LZClose(h)
    W16_CALL "LZEXPAND", 6
    push ds                         ; GetExpandedName("C:\WINDOWS\LZPROBE.TX_", lz_name)
    push word lz_path
    push ds
    push word lz_name
    W16_CALL "LZEXPAND", 10

    push word 300                   ; SetDoubleClickTime(300), then GetDoubleClickTime
    W16_CALL "USER", 20
    W16_CALL "USER", 21
    mov di, dbl_text
    call hex16
    mov byte [di], 0
    push word 1                     ; SwapMouseButton(TRUE): FALSE, then (FALSE): TRUE
    W16_CALL "USER", 186
    mov di, swap_text
    call hex16
    push di
    push word 0
    W16_CALL "USER", 186
    pop di
    call hex16
    mov byte [di], 0

    push cs                         ; EnumFontFamilies(screen, NULL, MakeProcInstance(famproc), 0)
    push word famproc
    push word [hinst]
    W16_CALL "KERNEL", 51
    push word [screen]
    push word 0
    push word 0
    push dx
    push ax
    push word 0
    push word 0
    W16_CALL "GDI", 330
    mov ax, [families]
    mov di, families_text
    call hex16
    mov byte [di], 0
    mov ax, [truetype]
    mov di, truetype_text
    call hex16
    mov byte [di], 0

    mov si, keys
.write:
    lodsw                           ; a key's name, then its text
    or ax, ax
    jz .written
    mov dx, ax
    lodsw
    push si                         ; kept across the call, under its arguments
    push ds                         ; WritePrivateProfileString("Probe", key, text, "W16PROBE.INI")
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

; The font `logfont` describes, selected into `mem`: GetTextExtent of `ext_str` (the digits), in hex at
; ES:DI.
extent:
    push di
    push ds                         ; CreateFontIndirect(&logfont)
    push word logfont
    W16_CALL "GDI", 57
    mov [font], ax
    push word [mem]
    push ax
    W16_CALL "GDI", 45              ; SelectObject(mem, font)
    mov [was], ax
    push word [mem]                 ; GetTextExtent(mem, ext_str, ext_len): AX the width
    push ds
    push word [ext_str]
    push word [ext_len]
    W16_CALL "GDI", 91
    mov [width], ax
    push word [mem]                 ; the old font back, the new one deleted
    push word [was]
    W16_CALL "GDI", 45
    push word [font]
    W16_CALL "GDI", 69
    pop di
    mov ax, [width]
    call hex16
    mov byte [di], 0
    ret

; int FAR PASCAL FamProc(lpelf, lpntm, FontType, lParam): each family counted, a TrueType one twice.
famproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    inc word [families]
    test word [bp+10], 4            ; TRUETYPE_FONTTYPE
    jz .counted
    inc word [truetype]
.counted:
    mov ax, 1
    xor dx, dx
    pop ds
    pop bp
    retf 14

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
W16_EXPORT 1, "FAMPROC", famproc

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            times 16 db 0           ; the Windows header words a task keeps
screen:     dw 0
hinst:      dw 0
families:   dw 0
truetype:   dw 0
maps:       dw 0x41, 0, map0_text, 0x1E, 1, map1_text, 0xBA, 2, map2_text, 0
mem:        dw 0
font:       dw 0
was:        dw 0
width:      dw 0
logfont:    dw -1000, 0, 0, 0, 400  ; height, width, escapement, orientation, weight
            db 0, 0, 0, 0, 0, 0, 0, 0x22    ; italic .. quality; VARIABLE_PITCH | FF_SWISS
            db "Arial"
            times 32 - 5 db 0
digits:     db "00:00:00"
dash:       db 0x97
ext_str:    dw digits
ext_len:    dw 8

keys:       dw key_base, base_text, key_async, async_text, key_w1000, wide1000_text
            dw key_w100, wide100_text, key_not, not_text, key_map0, map0_text, key_map1, map1_text
            dw key_map2, map2_text, key_fam, families_text, key_tt, truetype_text, key_dash, dash_text
            dw key_border, border_text, key_border2, border2_text, key_title, title_text
            dw key_ver, ver_text, key_verdest, verdest_text, key_vis, vis_text, key_lz, lz_text
            dw key_lzread, lzread_text, key_lzend, lzend_text, key_lztext, lz_buf, key_lzname, lz_name
            dw key_dbl, dbl_text, key_swap, swap_text, 0
key_base:   db "Base", 0
key_async:  db "Async", 0
key_w1000:  db "Wide1000", 0
key_w100:   db "Wide100", 0
key_not:    db "Not", 0
key_map0:   db "Map0", 0
key_map1:   db "Map1", 0
key_map2:   db "Map2", 0
key_fam:    db "Families", 0
key_tt:     db "TrueType", 0
key_dash:   db "Dash", 0
key_border: db "Border", 0
key_border2: db "Border2", 0
key_title:  db "Title", 0
key_ver:    db "Ver", 0
key_verdest: db "VerDest", 0
key_vis:    db "Vis", 0
key_lz:     db "LZ", 0
key_dbl:    db "Dbl", 0
key_swap:   db "Swap", 0
dbl_text:   times 6 db 0
swap_text:  times 10 db 0
key_lzread: db "LZRead", 0
key_lzend:  db "LZEnd", 0
key_lztext: db "LZText", 0
key_lzname: db "LZName", 0
lz_path:    db "C:\WINDOWS\LZPROBE.TX_", 0
lz_bare:    db "LZPROBE.TX_", 0
; "Hello, w16" as COMPRESS.EXE -r wrote it: SZDD's magic, method 'A', the 'T' the name lost, the
; length, then LZSS -- a flag byte (each bit a literal) before eight bytes, and one before the last two.
szdd:       db "SZDD", 0x88, 0xF0, 0x27, 0x33, "A", "T"
            dd 10
            db 0xFF, "Hello, w", 0x03, "16"
szdd_len    equ $ - szdd
lzh:        dw 0
lz_text:    times 6 db 0
lzread_text: times 6 db 0
lzend_text: times 10 db 0
lz_ofs:     times 136 db 0
lz_buf:     times 34 db 0
lz_name:    times 64 db 0
border_text: times 6 db 0
border2_text: times 6 db 0
title_text: times 6 db 0
ver_text:   times 6 db 0
verdest_text: times 6 db 0
vis_text:   times 10 db 0
value:      dw 0
title_lf:   times 50 db 0
nosuch:     db "NOSUCH.DLL", 0
cur_len:    dw 64
dest_len:   dw 64
ver_cur:    times 64 db 0
ver_dest:   times 64 db 0
rect_on:    dw 0, 0, 4, 1
rect_off:   dw 20, 20, 30, 30
dash_text:  times 6 db 0
map0_text:  times 6 db 0
map1_text:  times 6 db 0
map2_text:  times 6 db 0
families_text: times 6 db 0
truetype_text: times 6 db 0
base_text:  times 10 db 0
async_text: times 6 db 0
wide1000_text: times 6 db 0
wide100_text: times 6 db 0
not_text:   times 10 db 0
ini_app:    db "Probe", 0
ini_file:   db "W16PROBE.INI", 0
