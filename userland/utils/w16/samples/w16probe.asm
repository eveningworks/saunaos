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
;           Spacing= SPI_ICONHORIZONTALSPACING with no WIN.INI key: 3.11's Desktop dialog's 75 (004B) --
;                   Program Manager sizes each item from it, and its scroll range with them
;           Ver= / VerDest=  VerFindFile of a shared file no disk has: 0, and C:\WINDOWS\SYSTEM's length
;                   with its NUL (0012) -- Control Panel's applets ask it at start
;           Vis=    RectVisible on an 8x1 bitmap: a rectangle on it (0001), then one far off it (0000)
;           Dbl=    GetDoubleClickTime after SetDoubleClickTime(300): 012C
;           Swap=   SwapMouseButton(TRUE) then (FALSE): what each says the buttons were -- 00000001
;           Rgn=    a region from a RECT (10,10,20,20): PtInRegion at 15,15 (0001) and 25,15 (0000), OffsetRgn
;                   by 5,5's kind (0002, SIMPLEREGION), then GetRgnBox's left (000F)
;           Desk= / Sys=  GetSysColor(COLOR_BACKGROUND), 3.1's light grey (00C0C0C0); GetSysColor(
;                   COLOR_BTNFACE) after SetSysColors made it blue (00FF0000) for the run
;           Xor=    CombineRgn(RGN_XOR) of (0,0,10,10) and (5,5,15,15): its kind (0003, COMPLEXREGION), then
;                   PtInRegion at 7,7 (0000, in both) and 2,2 (0001)
;           Poly=   CreatePolygonRgn of the triangle (0,0) (20,0) (0,20): PtInRegion at 2,2 (0001) and 15,15 (0000)
;           Hole=   on the 8x1 bitmap: white, ExcludeClipRect(2,0,4,1), black: GetPixel at 3 (00FFFFFF, the
;                   hole kept white) and 6 (00000000)
;           LZ= / LZRead= / LZEnd= / LZText= / LZName=  LZEXPAND on a 10-byte SZDD file the probe writes to
;                   C:\WINDOWS and opens by its bare name -- OpenFile's search, as Control Panel opens
;                   CONTROL.INF: an LZ handle (0190, the first), the count read (000A), LZSeek to the end
;                   (0000000A), the expanded text, and GetExpandedName's LZPROBE.TXT
;           Multi=  an LBS_EXTENDEDSEL list of three: LB_GETSELCOUNT (0000), again after LB_SETSEL item 1
;                   and LB_SELITEMRANGE item 2 (0002), then LB_GETSELITEMS' two (0001, 0002) -- Fonts'
;                   list, whose sample needs a count of one
;           Single= LB_GETSELCOUNT of a single-selection list: LB_ERR (FFFF)
;           Var=    an LBS_OWNERDRAWVARIABLE list: item 1 made 30 tall with LB_SETITEMHEIGHT, then
;                   LB_GETITEMHEIGHT of it (001E) and item 2's top less item 0's height (001E) -- each
;                   row its own height, as Fonts' Sample draws 10, 12 and 15 point Courier
;           Raster= GetRasterizerCaps' flags: TrueType there and on (0003)
;           Font=   AddFontResource of a TrueType file (0001) and of a file that is no font (0000)
;           Clock=  DOS's date and time set and read back: 2Bh 2000-01-02 (00), 2Ah (07D0 0102), 2Bh the
;                   30th of February (FF), 2Dh 13:14:15 (00), 2Ch (0D0E); the clock put back after
;           FileRes= GetFileResourceSize of gdi.exe's bitmap 1, then the first word GetFileResource reads
;                   of it: a BITMAPINFOHEADER's size, 0028
;           Place=  GetWindowPlacement of the 100 x 60 popup list: the length (0016), SW_SHOWNORMAL (0001),
;                   and its normal rectangle's right and bottom (0064, 003C) -- Control Panel saves it as it
;                   closes
;           Disp=   CreateDC("DISPLAY", NULL, NULL, NULL) gives a DC (0001) -- the Windows Tutorial's first call
;           Cp=     with TA_UPDATECP, TextOut of "aa" after MoveTo(10, 0): how far the current position
;                   moved, then GetTextExtent of "aa" -- the two equal
;           Pop=    GetClientRect of the probe's first window, a WS_POPUP naming no border made 100 x 60:
;                   all of it (0064 003C)
;           Erase=  DefWindowProc(WM_ERASEBKGND) for a class with no background brush: 0, nothing erased,
;                   so BeginPaint's fErase stays TRUE
;           Kill=   PeekMessage for WM_TIMER after KillTimer, though the timer was due: none (0000)
;           Pal=    GetDIBits' 4-bit colour table, entries 7 and 15: 3.1's dark grey and white
;                   (00808080 00FFFFFF)
;           Calc=   DrawText DT_CALCRECT | DT_WORDBREAK of "aaaa b" in a box a pixel too narrow for it: its
;                   width, then GetTextExtent of "aaaa " -- the space the line broke at counted
;           Upd=    the probe's window invalidated whole, then its top half validated: GetUpdateRect's
;                   answer and box (0001, 0000 001E 0064 003C), then its answer once the bottom half is
;                   validated too (0000) -- an update region, not one mark a window
;           Obm=    LoadBitmap(NULL, OBM_REDUCE) with no display driver on drive C:: GetObject's width and
;                   height of w16's stand-in, VGA.DRV's 19 x 18 (0013 0012)
;           Grey=   an "O" in Arial at 24 pixels on white: how many of its pixels are neither black nor
;                   white -- some, smoothed; none with win16.conf's truetype=hard, as 3.1 drew TrueType
;           Jrnl=   a WH_JOURNALPLAYBACK hook handing back one WM_MOUSEMOVE to 30,40: how many times it was
;                   skipped past (0001), then GetCursorPos (001E 0028) -- played as real input
;           Mark=   DrawText of "&a&b" on a white bitmap: GetPixel's low word on the underline's row under
;                   the a, then under the b -- 3.1 underlined only a line's LAST prefixed letter (FFFF0000)
; Meant to run on real Windows 3.11 for Workgroups and on w16, the check of both: w16_gui_test.py reads
; the file. (On the reference VM it has stopped finishing -- docs/bugs.md.) Its windows stay hidden. Its
; name sorts after fault.exe: tests index the samples by their place.
; Build: tools/gen/gen_ne.py.
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

    ; the program's first window, before the list boxes: what the Windows Tutorial's main window is
    mov ax, [hinst]
    mov [pc_inst], ax
    push cs                         ; MakeProcInstance(defproc, hinst): what USER calls
    push word defproc
    push word [hinst]
    W16_CALL "KERNEL", 51
    mov [pc_proc], ax
    mov [pc_proc + 2], dx
    push ds
    push word pc
    W16_CALL "USER", 57             ; RegisterClass(&pc): a class with no background brush
    push ds                         ; CreateWindow("ProbeClass", "", WS_POPUP, 0, 0, 100, 60, ...)
    push word pc_name
    push ds
    push word empty
    push word 0x8000
    push word 0
    push word 0
    push word 0
    push word 100
    push word 60
    push word 0
    push word 0
    push word [hinst]
    push word 0
    push word 0
    W16_CALL "USER", 41
    mov [pw], ax
    push ax                         ; GetClientRect(pw, &item_rect): the first window, a WS_POPUP naming
    push ds                         ; no border -- all of its 100 x 60 is client
    push word item_rect
    W16_CALL "USER", 33
    mov di, pop_text
    mov ax, [item_rect + 4]
    call hex16
    mov ax, [item_rect + 6]
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
    push word 13                    ; SystemParametersInfo(SPI_ICONHORIZONTALSPACING, 0, &value, 0)
    push word 0
    push ds
    push word value
    push word 0
    W16_CALL "USER", 483
    mov ax, [value]
    mov di, spacing_text
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

    push ds                         ; CreateRectRgnIndirect(&rgn_rect)
    push word rgn_rect
    W16_CALL "GDI", 65
    mov [rgn], ax
    mov di, rgn_text
    push di
    push word [rgn]                 ; PtInRegion(rgn, 15, 15)
    push word 15
    push word 15
    W16_CALL "GDI", 161
    pop di
    call hex16
    push di
    push word [rgn]                 ; PtInRegion(rgn, 25, 15)
    push word 25
    push word 15
    W16_CALL "GDI", 161
    pop di
    call hex16
    push di
    push word [rgn]                 ; OffsetRgn(rgn, 5, 5)
    push word 5
    push word 5
    W16_CALL "GDI", 101
    pop di
    call hex16
    push di
    push word [rgn]                 ; GetRgnBox(rgn, &rgn_box): its left
    push ds
    push word rgn_box
    W16_CALL "GDI", 134
    pop di
    mov ax, [rgn_box]
    call hex16
    mov byte [di], 0
    push word [rgn]                 ; DeleteObject(rgn)
    W16_CALL "GDI", 69

    push word 1                     ; GetSysColor(COLOR_BACKGROUND): DX:AX
    W16_CALL "USER", 180
    mov di, desk_text
    push ax
    mov ax, dx
    call hex16
    pop ax
    call hex16
    mov byte [di], 0
    push word 1                     ; SetSysColors(1, &sys_index, &sys_rgb), then GetSysColor(COLOR_BTNFACE)
    push ds
    push word sys_index
    push ds
    push word sys_rgb
    W16_CALL "USER", 181
    push word 15
    W16_CALL "USER", 180
    mov di, sys_text
    push ax
    mov ax, dx
    call hex16
    pop ax
    call hex16
    mov byte [di], 0

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

    push word 0                     ; CreateRectRgn(0, 0, 10, 10) and (5, 5, 15, 15), then XOR them
    push word 0
    push word 10
    push word 10
    W16_CALL "GDI", 64
    mov [rgn], ax
    push word 5
    push word 5
    push word 15
    push word 15
    W16_CALL "GDI", 64
    mov [rgn2], ax
    push word [rgn]                 ; CombineRgn(rgn, rgn, rgn2, RGN_XOR)
    push word [rgn]
    push word [rgn2]
    push word 3
    W16_CALL "GDI", 47
    mov di, xor_text
    call hex16
    push di
    push word [rgn]
    push word 7
    push word 7
    W16_CALL "GDI", 161
    pop di
    call hex16
    push di
    push word [rgn]
    push word 2
    push word 2
    W16_CALL "GDI", 161
    pop di
    call hex16
    mov byte [di], 0
    push word [rgn]
    W16_CALL "GDI", 69
    push word [rgn2]
    W16_CALL "GDI", 69

    push ds                         ; CreatePolygonRgn(tri, 3, ALTERNATE)
    push word tri
    push word 3
    push word 1
    W16_CALL "GDI", 63
    mov [rgn], ax
    mov di, poly_text
    push di
    push word [rgn]
    push word 2
    push word 2
    W16_CALL "GDI", 161
    pop di
    call hex16
    push di
    push word [rgn]
    push word 15
    push word 15
    W16_CALL "GDI", 161
    pop di
    call hex16
    mov byte [di], 0
    push word [rgn]
    W16_CALL "GDI", 69

    push word [mem]                 ; PatBlt(mem, 0, 0, 8, 1, WHITENESS)
    push word 0
    push word 0
    push word 8
    push word 1
    push word 0x00FF
    push word 0x0062
    W16_CALL "GDI", 29
    push word [mem]                 ; ExcludeClipRect(mem, 2, 0, 4, 1)
    push word 2
    push word 0
    push word 4
    push word 1
    W16_CALL "GDI", 21
    push word [mem]                 ; PatBlt(mem, 0, 0, 8, 1, BLACKNESS)
    push word 0
    push word 0
    push word 8
    push word 1
    push word 0x0000
    push word 0x0042
    W16_CALL "GDI", 29
    push word [mem]                 ; GetPixel(mem, 3, 0): DX:AX
    push word 3
    push word 0
    W16_CALL "GDI", 83
    mov di, hole_text
    push ax
    mov ax, dx
    call hex16
    pop ax
    call hex16
    push di
    push word [mem]                 ; GetPixel(mem, 6, 0)
    push word 6
    push word 0
    W16_CALL "GDI", 83
    pop di
    push ax
    mov ax, dx
    call hex16
    pop ax
    call hex16
    mov byte [di], 0
    push word [mem]                 ; SelectClipRgn(mem, NULL): the clip taken away again
    push word 0
    W16_CALL "GDI", 44

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

    ; --- list boxes: Fonts' multiple selection, its Sample's rows of their own heights
    mov ax, 0x0800                  ; LBS_EXTENDEDSEL
    call listbox
    mov di, multi_text
    push word 0x0411                ; LB_GETSELCOUNT: none yet
    xor ax, ax
    xor dx, dx
    call lbsend
    call hex16
    push word 0x0406                ; LB_SETSEL(TRUE, 1)
    mov ax, 1
    mov dx, 1
    call lbsend
    push word 0x041C                ; LB_SELITEMRANGE(TRUE, MAKELONG(2, 2))
    mov ax, 1
    mov dx, 2
    call lbsend_hi2
    push word 0x0411                ; LB_GETSELCOUNT
    xor ax, ax
    xor dx, dx
    call lbsend
    call hex16
    push di
    push word [lb]                  ; LB_GETSELITEMS(10, sel_items)
    push word 0x0412
    push word 10
    push ds
    push word sel_items
    W16_CALL "USER", 111
    pop di
    mov ax, [sel_items]
    call hex16
    mov ax, [sel_items + 2]
    call hex16
    mov byte [di], 0
    xor ax, ax                      ; a single-selection list
    call listbox
    mov di, single_text
    push word 0x0411
    xor ax, ax
    xor dx, dx
    call lbsend
    call hex16
    mov byte [di], 0
    mov ax, 0x0060                  ; LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS
    call listbox
    mov di, var_text
    push word 0x0421                ; LB_SETITEMHEIGHT(1, 30)
    mov ax, 1
    mov dx, 30
    call lbsend
    push word 0x0422                ; LB_GETITEMHEIGHT(1)
    mov ax, 1
    xor dx, dx
    call lbsend
    call hex16
    push word 0x0422                ; LB_GETITEMHEIGHT(0)
    xor ax, ax
    xor dx, dx
    call lbsend
    mov [h0], ax
    push di
    push word [lb]                  ; LB_GETITEMRECT(2, item_rect)
    push word 0x0419
    push word 2
    push ds
    push word item_rect
    W16_CALL "USER", 111
    pop di
    mov ax, [item_rect + 2]
    sub ax, [h0]
    call hex16
    mov byte [di], 0

    push word [lb]                  ; GetWindowPlacement(the variable list, &place)
    push ds
    push word place
    W16_CALL "USER", 370
    mov di, place_text
    mov ax, [place]
    call hex16
    mov ax, [place + 4]
    call hex16
    mov ax, [place + 18]
    call hex16
    mov ax, [place + 20]
    call hex16
    mov byte [di], 0

    push ds                         ; GetRasterizerCaps(&raster, 6)
    push word raster
    push word 6
    W16_CALL "GDI", 313
    mov ax, [raster + 2]
    mov di, raster_text
    call hex16
    mov byte [di], 0

    push ds                         ; AddFontResource: a TrueType file, then one that is no font
    push word ttf_path
    W16_CALL "GDI", 119
    mov di, font_text
    call hex16
    push di
    push ds
    push word lz_path
    W16_CALL "GDI", 119
    pop di
    call hex16
    mov byte [di], 0

    mov ah, 0x2A                    ; DOS's date and time, kept to put back
    int 0x21
    mov [old_date], cx
    mov [old_date + 2], dx
    mov ah, 0x2C
    int 0x21
    mov [old_time], cx
    mov [old_time + 2], dx
    mov di, clock_text
    mov ah, 0x2B                    ; 2000-01-02
    mov cx, 2000
    mov dx, 0x0102
    int 0x21
    call hex8
    mov ah, 0x2A
    int 0x21
    mov ax, cx
    call hex16
    mov ax, dx
    call hex16
    mov ah, 0x2B                    ; the 30th of February
    mov cx, 2001
    mov dx, 0x021E
    int 0x21
    call hex8
    mov ah, 0x2D                    ; 13:14:15
    mov cx, 0x0D0E
    mov dx, 0x0F00
    int 0x21
    call hex8
    mov ah, 0x2C
    int 0x21
    mov ax, cx
    call hex16
    mov byte [di], 0
    mov ah, 0x2B                    ; the clock as it was
    mov cx, [old_date]
    mov dx, [old_date + 2]
    int 0x21
    mov ah, 0x2D
    mov cx, [old_time]
    mov dx, [old_time + 2]
    int 0x21

    push ds                         ; GetFileResourceSize(gdi.exe, RT_BITMAP, 1, &offset)
    push word gdi_path
    push word 0
    push word 2
    push word 0
    push word 1
    push ds
    push word res_off
    W16_CALL "VER", 2
    mov di, fileres_text
    push ax
    mov ax, dx
    call hex16
    pop ax
    call hex16
    push di
    push ds                         ; GetFileResource(gdi.exe, RT_BITMAP, 1, 0, 4, res_buf)
    push word gdi_path
    push word 0
    push word 2
    push word 0
    push word 1
    push word 0
    push word 0
    push word 0
    push word 4
    push ds
    push word res_buf
    W16_CALL "VER", 3
    pop di
    mov ax, [res_buf]
    call hex16
    mov byte [di], 0

    ; CreateDC("DISPLAY") and a borderless popup's client, then what the Windows Tutorial leans on
    push ds                         ; CreateDC("DISPLAY", NULL, NULL, NULL): the screen's
    push word display
    push word 0
    push word 0
    push word 0
    push word 0
    push word 0
    push word 0
    W16_CALL "GDI", 53
    or ax, ax
    jz .nodisp
    push ax
    W16_CALL "GDI", 68              ; DeleteDC
    mov ax, 1
.nodisp:
    mov di, disp_text
    call hex16
    mov byte [di], 0

    push word [mem]                 ; MoveTo(mem, 10, 0)
    push word 10
    push word 0
    W16_CALL "GDI", 20
    push word [mem]                 ; SetTextAlign(mem, TA_UPDATECP)
    push word 1
    W16_CALL "GDI", 346
    push word [mem]                 ; TextOut(mem, 0, 0, "ab", 2): at the current position, moved past it
    push word 0
    push word 0
    push ds
    push word calc_str
    push word 2
    W16_CALL "GDI", 33
    push word [mem]                 ; GetCurrentPosition(mem): AX its x
    W16_CALL "GDI", 78
    sub ax, 10
    mov di, cp_text
    call hex16
    push di
    push word [mem]                 ; GetTextExtent(mem, "aa", 2)
    push ds
    push word calc_str
    push word 2
    W16_CALL "GDI", 91
    pop di
    call hex16
    mov byte [di], 0
    push word [mem]                 ; SetTextAlign(mem, TA_LEFT | TA_TOP)
    push word 0
    W16_CALL "GDI", 346

    push word [pw]                  ; DefWindowProc(pw, WM_ERASEBKGND, mem, 0): no brush, nothing erased
    push word 0x0014
    push word [mem]
    push word 0
    push word 0
    W16_CALL "USER", 107
    mov di, erase_text
    call hex16
    mov byte [di], 0

    push word [pw]                  ; SetTimer(pw, 7, 1, NULL), then 300 ms with no message taken
    push word 7
    push word 1
    push word 0
    push word 0
    W16_CALL "USER", 10
    W16_CALL "USER", 13             ; GetTickCount
    mov [t0], ax
    mov word [spins], 0
.tick:
    W16_CALL "USER", 13
    sub ax, [t0]
    cmp ax, 300
    jae .ticked
    dec word [spins]                ; bounded: at most 65536 looks at the clock
    jnz .tick
.ticked:
    push word [pw]                  ; KillTimer(pw, 7)
    push word 7
    W16_CALL "USER", 12
    push ds                         ; PeekMessage(&pmsg, pw, WM_TIMER, WM_TIMER, PM_REMOVE): none after it
    push word pmsg
    push word [pw]
    push word 0x0113
    push word 0x0113
    push word 1
    W16_CALL "USER", 109
    mov di, kill_text
    call hex16
    mov byte [di], 0

    push word [pw]                  ; InvalidateRect(pw, NULL, FALSE), then ValidateRect of its top half
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 125
    push word [pw]
    push ds
    push word upd_top
    W16_CALL "USER", 127
    push word [pw]                  ; GetUpdateRect(pw, &item_rect, FALSE): the bottom half is left
    push ds
    push word item_rect
    push word 0
    W16_CALL "USER", 190
    mov di, upd_text
    call hex16
    mov si, item_rect
    mov cx, 4
.updbox:
    lodsw
    push cx
    call hex16
    pop cx
    loop .updbox
    push di
    push word [pw]                  ; ValidateRect of the bottom half: nothing owed
    push ds
    push word upd_bottom
    W16_CALL "USER", 127
    push word [pw]
    push word 0
    push word 0
    push word 0
    W16_CALL "USER", 190
    pop di
    call hex16
    mov byte [di], 0

    push word [screen]              ; CreateCompatibleBitmap(screen, 8, 1)
    push word 8
    push word 1
    W16_CALL "GDI", 51
    mov [dib_bm], ax
    push word [screen]              ; GetDIBits(screen, bitmap, 0, 1, dib_bits, &dib_bmi, DIB_RGB_COLORS)
    push ax
    push word 0
    push word 1
    push ds
    push word dib_bits
    push ds
    push word dib_bmi
    push word 0
    W16_CALL "GDI", 441
    push word [dib_bm]
    W16_CALL "GDI", 69              ; DeleteObject
    mov di, pal_text                ; the table's entries 7 and 15: 3.1's dark grey and white
    mov ax, [dib_bmi + 40 + 7 * 4 + 2]
    call hex16
    mov ax, [dib_bmi + 40 + 7 * 4]
    call hex16
    mov ax, [dib_bmi + 40 + 15 * 4 + 2]
    call hex16
    mov ax, [dib_bmi + 40 + 15 * 4]
    call hex16
    mov byte [di], 0

    push word [mem]                 ; GetTextExtent(mem, "aaaa b", 6), less 1: "aaaa b" must wrap in it
    push ds
    push word calc_str
    push word 6
    W16_CALL "GDI", 91
    dec ax
    mov [calc_rc + 4], ax
    push word [mem]                 ; DrawText(mem, "aaaa b", 6, &calc_rc, DT_CALCRECT | DT_WORDBREAK)
    push ds
    push word calc_str
    push word 6
    push ds
    push word calc_rc
    push word 0x0410
    W16_CALL "USER", 85
    mov di, calc_text               ; its width, then "aaaa "'s: the space it broke at counted
    mov ax, [calc_rc + 4]
    call hex16
    push di
    push word [mem]
    push ds
    push word calc_str
    push word 5
    W16_CALL "GDI", 91
    pop di
    call hex16
    mov byte [di], 0

    push word 40                    ; CreateBitmap(40, 20, 1, 1, NULL): "&a&b" drawn on it in black
    push word 20
    push word 1
    push word 1
    push word 0
    push word 0
    W16_CALL "GDI", 48
    mov [mark_bm], ax
    push word [mem]
    push ax
    W16_CALL "GDI", 45              ; SelectObject(mem, bitmap)
    mov [mark_was], ax
    push word [mem]                 ; PatBlt(mem, 0, 0, 40, 20, WHITENESS)
    push word 0
    push word 0
    push word 40
    push word 20
    push word 0x00FF
    push word 0x0062
    W16_CALL "GDI", 29
    push word [mem]                 ; DrawText(mem, "&a&b", 4, &mark_rc, 0)
    push ds
    push word mark_str
    push word 4
    push ds
    push word mark_rc
    push word 0
    W16_CALL "USER", 85
    push word [mem]                 ; GetTextMetrics(mem, &tm_buf): the underline is a row under the ascent
    push ds
    push word tm_buf
    W16_CALL "GDI", 93
    push word [mem]                 ; GetTextExtent(mem, "a", 1): where the b starts
    push ds
    push word mark_str + 1
    push word 1
    W16_CALL "GDI", 91
    mov [mark_x], ax
    mov di, mark_text
    mov cx, 1                       ; GetPixel under the a, then under the b
.marks:
    push cx
    push di
    push word [mem]
    push cx
    mov ax, [tm_buf + 2]
    inc ax
    push ax
    W16_CALL "GDI", 83
    pop di
    call hex16
    pop cx
    cmp cx, 1
    jne .marked
    mov cx, [mark_x]
    inc cx
    jmp .marks
.marked:
    mov byte [di], 0
    push word [mem]
    push word [mark_was]
    W16_CALL "GDI", 45              ; the old bitmap back
    push word [mark_bm]
    W16_CALL "GDI", 69

    push word 0                     ; LoadBitmap(NULL, OBM_REDUCE): no display driver on drive C: here,
    push word 0                     ; so w16's own drawing of it -- 19 x 18, as VGA.DRV's
    push word 32749
    W16_CALL "USER", 175
    mov [mark_bm], ax
    push ax                         ; GetObject(bitmap, 14, &tm_buf): a BITMAP
    push word 14
    push ds
    push word tm_buf
    W16_CALL "GDI", 82
    mov di, obm_text
    mov ax, [tm_buf + 2]
    call hex16
    mov ax, [tm_buf + 4]
    call hex16
    mov byte [di], 0
    push word [mark_bm]
    W16_CALL "GDI", 69

    mov word [logfont], -24         ; an "O" in Arial at 24 pixels on a white 32 x 32 bitmap: how many
    push ds                         ; of its pixels are neither black nor white -- none with the
    push word logfont               ; `truetype` setting at Hard
    W16_CALL "GDI", 57              ; CreateFontIndirect
    mov [font], ax
    push word [screen]
    push word 32
    push word 32
    W16_CALL "GDI", 51              ; CreateCompatibleBitmap(screen, 32, 32)
    mov [mark_bm], ax
    push word [mem]
    push ax
    W16_CALL "GDI", 45
    mov [mark_was], ax
    push word [mem]
    push word [font]
    W16_CALL "GDI", 45
    mov [was], ax
    push word [mem]                 ; PatBlt(mem, 0, 0, 32, 32, WHITENESS)
    push word 0
    push word 0
    push word 32
    push word 32
    push word 0x00FF
    push word 0x0062
    W16_CALL "GDI", 29
    push word [mem]                 ; TextOut(mem, 4, 0, "O", 1)
    push word 4
    push word 0
    push ds
    push word grey_o
    push word 1
    W16_CALL "GDI", 33
    mov word [greys], 0
    mov word [gy], 0
.grow:
    mov word [gx], 0
.gcol:
    push word [mem]
    push word [gx]
    push word [gy]
    W16_CALL "GDI", 83              ; GetPixel
    and dx, 0x00FF
    mov cx, ax
    or cx, dx
    jz .gnext                       ; black
    cmp ax, 0xFFFF
    jne .ggrey
    cmp dx, 0x00FF
    je .gnext                       ; white
.ggrey:
    inc word [greys]
.gnext:
    inc word [gx]
    cmp word [gx], 32
    jb .gcol
    inc word [gy]
    cmp word [gy], 32
    jb .grow
    mov di, grey_text
    mov ax, [greys]
    call hex16
    mov byte [di], 0
    push word [mem]
    push word [was]
    W16_CALL "GDI", 45
    push word [mem]
    push word [mark_was]
    W16_CALL "GDI", 45
    push word [font]
    W16_CALL "GDI", 69
    push word [mark_bm]
    W16_CALL "GDI", 69

    push cs                         ; WH_JOURNALPLAYBACK: one mouse move to 30,40 played back
    push word jrnlproc
    push word [hinst]
    W16_CALL "KERNEL", 51           ; MakeProcInstance(jrnlproc, hinst)
    mov [jrnl_fn], ax
    mov [jrnl_fn + 2], dx
    push word 1                     ; SetWindowsHook(WH_JOURNALPLAYBACK, jrnl_fn)
    push dx
    push ax
    W16_CALL "USER", 121
    W16_CALL "USER", 13             ; GetTickCount
    mov [t0], ax
.jrnl:
    cmp word [jrnl_skips], 0        ; played once it was skipped past -- or two seconds gone
    jne .jrnled
    push ds                         ; PeekMessage(&pmsg, NULL, 0, 0, PM_REMOVE): where it plays
    push word pmsg
    push word 0
    push word 0
    push word 0
    push word 1
    W16_CALL "USER", 109
    W16_CALL "USER", 13
    sub ax, [t0]
    cmp ax, 2000
    jb .jrnl
.jrnled:
    push word 1                     ; UnhookWindowsHook(WH_JOURNALPLAYBACK, jrnl_fn)
    push word [jrnl_fn + 2]
    push word [jrnl_fn]
    W16_CALL "USER", 234
    push ds                         ; GetCursorPos(&item_rect): where the played move put it
    push word item_rect
    W16_CALL "USER", 17
    mov di, jrnl_text
    mov ax, [jrnl_skips]
    call hex16
    mov ax, [item_rect]
    call hex16
    mov ax, [item_rect + 2]
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

; A hidden popup LISTBOX of style AX (its low word), holding "a", "b" and "c": its handle in [lb].
listbox:
    push di
    push ds                         ; CreateWindow("LISTBOX", "", WS_POPUP | style, 0, 0, 100, 60, ...)
    push word lb_class
    push ds
    push word empty
    push word 0x8000
    push ax
    push word 0
    push word 0
    push word 100
    push word 60
    push word 0
    push word 0
    push word [hinst]
    push word 0
    push word 0
    W16_CALL "USER", 41
    mov [lb], ax
    mov si, lb_items
.add:
    lodsw
    or ax, ax
    jz .added
    push si
    push word [lb]                  ; LB_ADDSTRING(0, item)
    push word 0x0401
    push word 0
    push ds
    push ax
    W16_CALL "USER", 111
    pop si
    jmp .add
.added:
    pop di
    ret

; SendMessage([lb], the message pushed by the caller, AX, DX as lParam's low word): AX the answer.
lbsend:
    pop bx                          ; the return, under the caller's message
    pop cx
    push bx
    push di
    push word [lb]
    push cx
    push ax
    push word 0
    push dx
    W16_CALL "USER", 111
    pop di
    ret
; ...and DX as both of lParam's words: MAKELONG(DX, DX), LB_SELITEMRANGE's first and last.
lbsend_hi2:
    pop bx
    pop cx
    push bx
    push di
    push word [lb]
    push cx
    push ax
    push dx
    push dx
    W16_CALL "USER", 111
    pop di
    ret

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

; LONG FAR PASCAL DefProc(hwnd, msg, wParam, lParam): DefWindowProc's, for the probe's own class. It
; reads no data, so it leaves DS alone: USER calls a class's procedure with no DGROUP in AX.
defproc:
    push bp
    mov bp, sp
    push word [bp+14]
    push word [bp+12]
    push word [bp+10]
    push word [bp+8]
    push word [bp+6]
    W16_CALL "USER", 107
    pop bp
    retf 10

; DWORD FAR PASCAL JrnlProc(code, wParam, lParam): HC_GETNEXT hands back a WM_MOUSEMOVE to 30,40 and
; no wait; HC_SKIP counts.
jrnlproc:
    push bp
    mov bp, sp
    push ds
    mov ds, ax
    cmp word [bp+12], 1             ; HC_GETNEXT: the EVENTMSG at lParam
    jne .skip
    push es
    push di
    les di, [bp+6]
    mov word [es:di], 0x0200
    mov word [es:di+2], 30
    mov word [es:di+4], 40
    mov word [es:di+6], 0
    mov word [es:di+8], 0
    pop di
    pop es
    jmp .done
.skip:
    cmp word [bp+12], 2             ; HC_SKIP
    jne .done
    inc word [jrnl_skips]
.done:
    xor ax, ax
    xor dx, dx
    pop ds
    pop bp
    retf 8

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
W16_EXPORT 2, "DEFPROC", defproc
W16_EXPORT 3, "JRNLPROC", jrnlproc

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
            dw key_spacing, spacing_text
            dw key_ver, ver_text, key_verdest, verdest_text, key_vis, vis_text, key_lz, lz_text
            dw key_lzread, lzread_text, key_lzend, lzend_text, key_lztext, lz_buf, key_lzname, lz_name
            dw key_dbl, dbl_text, key_swap, swap_text, key_rgn, rgn_text, key_desk, desk_text
            dw key_sys, sys_text, key_xor, xor_text, key_poly, poly_text, key_hole, hole_text
            dw key_multi, multi_text, key_single, single_text, key_var, var_text, key_raster, raster_text
            dw key_font, font_text, key_clock, clock_text, key_fileres, fileres_text, key_place, place_text
            dw key_disp, disp_text, key_cp, cp_text, key_pop, pop_text, key_erase, erase_text
            dw key_kill, kill_text, key_pal, pal_text, key_calc, calc_text, key_mark, mark_text
            dw key_upd, upd_text, key_obm, obm_text, key_grey, grey_text
            dw key_jrnl, jrnl_text, 0
key_disp:   db "Disp", 0
key_cp:     db "Cp", 0
key_pop:    db "Pop", 0
key_erase:  db "Erase", 0
key_kill:   db "Kill", 0
key_pal:    db "Pal", 0
key_calc:   db "Calc", 0
key_mark:   db "Mark", 0
key_upd:    db "Upd", 0
key_obm:    db "Obm", 0
key_grey:   db "Grey", 0
key_jrnl:   db "Jrnl", 0
jrnl_text:  times 14 db 0
jrnl_fn:    dd 0
jrnl_skips: dw 0
grey_text:  times 6 db 0
grey_o:     db "O"
greys:      dw 0
gx:         dw 0
gy:         dw 0
obm_text:   times 10 db 0
upd_text:   times 26 db 0
upd_top:    dw 0, 0, 100, 30
upd_bottom: dw 0, 30, 100, 60
mark_text:  times 10 db 0
mark_str:   db "&a&b"
mark_rc:    dw 0, 0, 40, 20
mark_bm:    dw 0
mark_was:   dw 0
mark_x:     dw 0
tm_buf:     times 32 db 0
disp_text:  times 6 db 0
cp_text:    times 10 db 0
pop_text:   times 10 db 0
erase_text: times 6 db 0
kill_text:  times 6 db 0
pal_text:   times 18 db 0
calc_text:  times 10 db 0
display:    db "DISPLAY", 0
calc_str:   db "aaaa b"
calc_rc:    dw 0, 0, 0, 100
pc:                                 ; WNDCLASS: no icon, cursor or background brush
            dw 0
pc_proc:    dd 0                    ; lpfnWndProc: MakeProcInstance's
            dw 0, 0
pc_inst:    dw 0
            dw 0, 0, 0
            dd 0
            W16_FARPTR pc_name, 2
pc_name:    db "ProbeClass", 0
pw:         dw 0
t0:         dw 0
spins:      dw 0
pmsg:       times 18 db 0
dib_bm:     dw 0
dib_bmi:    dd 40, 8, 1             ; BITMAPINFOHEADER: size, width, height
            dw 1, 4                 ; planes, 4 bits a pixel
            dd 0, 0, 0, 0, 0, 0
            times 16 dd 0           ; the colour table GetDIBits fills
dib_bits:   times 8 db 0
key_place:  db "Place", 0
place_text: times 18 db 0
place:      times 11 dw 0
key_multi:  db "Multi", 0
key_single: db "Single", 0
key_var:    db "Var", 0
key_raster: db "Raster", 0
key_font:   db "Font", 0
key_clock:  db "Clock", 0
key_fileres: db "FileRes", 0
multi_text: times 18 db 0
single_text: times 6 db 0
var_text:   times 10 db 0
raster_text: times 6 db 0
font_text:  times 10 db 0
clock_text: times 20 db 0
fileres_text: times 14 db 0
lb:         dw 0
h0:         dw 0
sel_items:  times 10 dw 0
item_rect:  dw 0, 0, 0, 0
raster:     dw 0, 0, 0
old_date:   dw 0, 0
old_time:   dw 0, 0
res_off:    dd 0
res_buf:    dw 0, 0
lb_class:   db "LISTBOX", 0
empty:      db 0
lb_items:   dw item_a, item_b, item_c, 0
item_a:     db "a", 0
item_b:     db "b", 0
item_c:     db "c", 0
ttf_path:   db "Z:\usr\share\fonts\liberation-sans.ttf", 0
gdi_path:   db "Z:\usr\share\w16\gdi.exe", 0
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
key_spacing: db "Spacing", 0
key_title:  db "Title", 0
key_ver:    db "Ver", 0
key_verdest: db "VerDest", 0
key_vis:    db "Vis", 0
key_lz:     db "LZ", 0
key_dbl:    db "Dbl", 0
key_rgn:    db "Rgn", 0
key_xor:    db "Xor", 0
key_poly:   db "Poly", 0
key_hole:   db "Hole", 0
xor_text:   times 14 db 0
poly_text:  times 10 db 0
hole_text:  times 18 db 0
rgn2:       dw 0
tri:        dw 0, 0, 20, 0, 0, 20
key_desk:   db "Desk", 0
key_sys:    db "Sys", 0
rgn_text:   times 18 db 0
desk_text:  times 10 db 0
sys_text:   times 10 db 0
rgn:        dw 0
rgn_rect:   dw 10, 10, 20, 20
rgn_box:    times 4 dw 0
sys_index:  dw 15                   ; COLOR_BTNFACE
sys_rgb:    dd 0x00FF0000           ; a COLORREF: blue
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
spacing_text: times 6 db 0
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
