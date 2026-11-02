#!/usr/bin/env python3
"""w16 running Win16 programs in toywm (docs/w16-design.md), checked by pixel.

The programs are the project's own NASM samples (userland/w16/samples/,
built by tools/gen/gen_ne.py), so the test ships with the tree; no Microsoft
file is involved.

1. **gdi.exe draws everything where it put it** (its header lists the
   places): a string-table string, a bitmap resource put with
   SetDIBitsToDevice and through a memory DC with BitBlt -- SRCCOPY the
   same colours, NOTSRCCOPY every one inverted (by palette index, as 3.11 did), and the DIB's rows the
   right way up -- a pen line that stops short of its end point, a brush
   rectangle inside a pen edge, text copied through the global and local
   heaps, a square that appears only once its timer has fired three
   times; and a metafile played at twice its size through MM_ANISOTROPIC
   (PlayMetaFile, lib/uwmf: a NULL-pen rectangle and an ellipse), a PatBlt
   cut by IntersectClipRect inside SaveDC/RestoreDC, and a Polygon.
2. **The window controls**: Maximize's arrow sits a row higher than
   Minimize's, as 3.11 drew them; Alt+Space opens w16's own control menu (a
   popup), X maximizes -- the window takes the work area and the frame
   loses its border, so the caption reaches the top-left pixel -- and
   Alt+Space, Enter restores the size it had.
3. **A fault is told**: fault.exe's click is a general protection fault;
   the Application Error box opens, Enter closes it, the program ends,
   and its crash report's message names the fault and where.
4. **INI files, both ways**: gdi.exe draws a magenta square only when
   GDITEST.INI (written here first, in drive C:'s WINDOWS folder) says
   Square=1, and writes Written=yes to it itself.
5. **A program's own settings** (/etc/win16/<program>.conf, w16prefs.h):
   scale=2 opens gdi.exe at exactly twice its size, scale=1.5 at exactly
   1.5 times with method=pixels leaving only pure colours across an edge
   and method=smooth blending them, and frame=desktop gives hello.exe
   toywm's frame with the client still the size the program asked for.
6. **Retro Launcher**: the launcher lists the samples, a double click on
   one runs it, and `retrolauncher FILE` runs a program with no launcher window
   (the File Manager's Open for an .exe).
8. **Dialogs and the menu bar** (dialog.exe): Alt+F, C ticks File >
   Checked off (CheckMenuItem; Checked=0 lands in DIALOG.INI), F2 is an
   accelerator for File > Number..., whose dialog starts at 42 and keeps
   what is typed over it (Value=), a right click's MessageBox answers No
   with its mnemonic (Box=7), and Help > About is ShellAbout's box. A second
   GetDC while one is held is another DC, the first keeping its pen (NestedDC=1).
7. **A program's calls** (`retrolauncher --calls FILE`, retrolauncher/calls.c): fault.exe
   lacks exactly USER.7 ExitWindows and has one stand-in, IsIconic
   (`w16 --check` says `partly 1` and why), so its window opens on the
   Missing chip with that one row under two module chips (All, USER);
   Partly lists IsIconic; All lists every import `w16 --check` counts;
   a search narrows it to one; Ctrl+C copies the rows shown; hello.exe,
   which lacks nothing, opens on All, and gdi.exe -- nothing missing, one
   stand-in (PlayMetaFile) -- on Partly; and the launcher's details panel
   opens the window for the selected program.
9. **The pointer under a capture** (hello.exe's scrolling edit): an
   I-beam over the text, an arrow over the scroll bar, and dragging the
   thumb onto the text keeps the arrow until the button is let go.
10. **Drives and folders**: drive D: mapped to /home/w16d gets hello.exe's
   D:\\HELLO.TXT; nine folders in /etc/win16.conf make ten sidebar rows (the
   list had room for eight); a folder's right-click Move down is saved.
11. **From the File Manager** (`retrolauncher FILE`, `--details`, the desktop
   entry's actions): hello.exe runs at once, fault.exe (a call missing)
   gets the run card, an MZ file that is not Windows 3.x gets "cannot run
   here", `from_files=ask` cards hello.exe too, `--details` names the
   rows, and the File Manager's right-click on fault.exe lists Show its
   calls under Open, which opens the calls window.
12. **The details panel and Options** (`--show`): in a short window
   fault.exe's Run sits in the pinned footer and its Drive C: row is
   scrolled out; the wheel brings it into view above Run; the Options
   window takes a bigger size, and a smaller one with its page scrolling,
   but has a floor.
13. **Options > Sound**: Windows 3.11's seven events with no WIN.INI on
   drive C:; the first row's Play plays SaunaOS's chime, and nothing once
   the Sounds choice is Off (before OK).
14. **Why?** on a stand-in: IsIconic's cell in fault.exe's calls window
   and its link in the details panel each open "IsIconic - partly there"
   with its section of the calls page, and a working call's About opens
   "<name> - about"; a Missing row's What? opens "<name> - not there yet"
   with its section of the missing calls' page, and so does the panel's
   What? link; `w16 --why` prints a stand-in's section, a missing call's,
   and the made-up card for a missing call with no text or no name.

15. **The coprocessor** (fpu.exe, cpu16_x87.c + win87em16.c): with _fpMath's
   control word, 1/0 and sqrt(-1) each reach the program's handler once,
   as 83h and 81h, and the FWAIT after them runs on; a ninth load is a
   stack overflow, 8Ah; masked, 1/0 calls nothing and BX=8 reports the
   zero divide; sqrt(2) * 1e9 stored by FISTP is 1414213562 exactly.
   UNDER TCG the x87 is QEMU's, which has no stack fault: the test asks
   QEMU (`info kvm`) and expects no 8Ah there, and exactly 8Ah under KVM
   and on bare metal (`--host`).

16. **GDI's curves as 3.11 drew them** (shapes.exe, gdi16.c): Ellipse(150, 10,
   210, 50)'s rows start in exactly as many pixels as real 3.11's did,
   RoundRect's 20 x 16 corners likewise (the midpoint ellipse both), a
   pie's vertical radius steps across a pixel where 3.11's did, and its
   brush reaches the radius with no gap.

17. **Calls that need no window** (wincalls.exe): huge GlobalAlloc and GlobalReAlloc, GetDIBits,
   Get/SetBitmapBits, ExtFloodFill on a 1-bit bitmap, INT 2Fh, EqualRect, AnsiUpperBuff, wvsprintf,
   the Windows and System directories, GlobalLRUNewest, the colour and text-align
   state of a device context, CreatePalette -- each answer written to WINCALLS.INI
   and compared with what Windows 3.11 answers.

18. **The Open dialog on a program's own template** (opendlg.exe): Read Only and Help hidden or
   shown by the flags, and Read Only's box as OK found it handed back in OFN_READONLY.

19. **Retro Launcher's missing calls, trace, INI file and drives** (`retro_round`): `missing=zero` in
   fault.exe's own settings answers ExitWindows 0 and its record names the call; the details panel has the
   Missing calls row and its footer says "1 missing call / answered 0 -- which?", whose link scrolls the
   panel to them; the run card's "Run, skipping them" runs it so this once; the split Run button's arrow
   opens its three ways to run above it, Run with trace records the trace and the stop, and the footer's
   Open the trace opens Log Viewer; its INI file is "None yet" until Make one... makes it (and opens
   Notepad), then its first lines; a CD-ROM drive is a sidebar row under Drives listing the program in its
   subfolder, and w16's trace names its label_d; Options has When a call is missing, and Drives saves a
   drive's Type and Label. No footer control sticks out of the panel or overlaps another.
20. **The screen effect** (`screen_effect`, w16.c + ucrt): gdi.exe at 2x with scanlines in its own
   settings keeps its 3.1 caption pixel for pixel and darkens the second screen line of every 3.1 row
   in its client, where plain the two match; Alt+Space, E opens the Screen effect cascade beside the
   control menu, its last row lights under the pointer at once, the kept effect is drawn again when the
   menus close, O chooses Off -- saved to its own file by hlpbroker -- and the client is plain again,
   and then Custom (its look is no preset) turns that look back on.

Every colour is compared with a tolerance of 0: the client buffer is
32-bit and nothing between it and the screendump blends.
"""

RUNNER = {
    "name": "w16",
    "covers": "Win16 programs: GDI and resources by pixel, window controls, a fault",
}

import argparse
import re
import sys
import time

import os  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
import filemanager_test as fm  # noqa: E402
from harness import Results, poll  # noqa: E402

GDI_TITLE = "GDI test (Win16)"
HELLO_TITLE = "Hello (Win16)"
INI = "/home/Win16/WINDOWS/GDITEST.INI"
GDI_CONF = "/etc/win16/usr_share_w16_gdi.exe.conf"
POPUP_TITLE = "Popup test"
HELLO_CONF = "/etc/win16/usr_share_w16_hello.exe.conf"
LAST = "/var/lib/win16/usr_share_w16_{}.exe.last"   # what the launcher's "Last run" reads
FAULT_TITLE = "Fault (Win16)"
FRAME, CAPTION = 4, 19     # userland/w16/w16_internal.h's W16_FRAME, W16_CAPTION

RED, GREEN, BLUE, WHITE = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)
ODD = (200, 100, 50)        # gdi.exe's brush of a colour the 16-colour VGA has not got
YELLOW, CYAN, MAGENTA, BLACK = (255, 255, 0), (0, 255, 255), (255, 0, 255), (0, 0, 0)
# w16 shows 3.11's colours as VGA did (ctl16.c's w16_vga): navy and the face grey shift.
NAVY, FACE = (0, 0, 168), (192, 199, 200)


def shot(qmp, path):
    from PIL import Image
    qmp.screenshot(path)
    with Image.open(path) as im:
        return im.convert("RGB")


# A colour inverted as the display did it: its palette index's complement, not its RGB's (blit16.c).
# Measured on real 3.11 (2026-10-22): Paintbrush's selected tool, a DSTINVERT, turns dark cyan red,
# magenta dark green and dark magenta green. The eight here, shown as VGA showed the dark ones.
INVERSE = {RED: (87, 168, 168), GREEN: (168, 87, 168), BLUE: (168, 168, 87), WHITE: BLACK,
           YELLOW: NAVY, CYAN: (168, 0, 87), MAGENTA: (0, 168, 87), BLACK: WHITE}


def inv(c):
    return INVERSE[c]


def sh(dbg, cmd):
    return dbg.send(f"sh {cmd}") or ""


def probe(dbg, res):
    """w16probe.exe's answers (userland/w16/samples/w16probe.asm), against Windows 3.1's: the dialog
    base units at VGA, no button down, a 1000-pixel font measuring ten times a 100-pixel one (Clock
    sizes its digits so), and R2_NOT on light grey giving the palette's dark grey (Clock's second hand)."""
    sh(dbg, "mkdir /home/Win16")
    sh(dbg, "mkdir /home/Win16/WINDOWS")
    sh(dbg, "rm /home/Win16/WINDOWS/W16PROBE.INI")
    sh(dbg, "w16 /usr/share/w16/w16probe.exe")
    out = sh(dbg, "cat /home/Win16/WINDOWS/W16PROBE.INI")
    got = dict(re.findall(r"(\w+)=([0-9A-F]+)", out))
    res.check("GetDialogBaseUnits answers 3.1's VGA units, 8 by 16", got.get("Base") == "00100008", got)
    res.check("GetAsyncKeyState says the left button is up", got.get("Async") == "0000", got)
    w1000, w100 = int(got.get("Wide1000", "0"), 16), int(got.get("Wide100", "0"), 16)
    res.check("a 1000-pixel font measures ten times a 100-pixel one (not cut at what ugfx can draw)",
              w100 > 0 and 9.5 <= w1000 / w100 <= 10.5, f"{w1000} / {w100}")
    res.check("R2_NOT on light grey is dark grey, as on 3.1's palette, and GetPixel says 0x808080",
              got.get("Not") == "00808080", got)
    res.check("MapVirtualKey: VK_A to scan 1E, scan 1E to VK_A, VK_OEM_1 to ';'",
              (got.get("Map0"), got.get("Map1"), got.get("Map2")) == ("001E", "0041", "003B"), got)
    fam, tt = int(got.get("Families", "0"), 16), int(got.get("TrueType", "0"), 16)
    res.check("EnumFontFamilies calls back once per family, the TrueType ones flagged (Character Map's list)",
              fam > tt > 0, f"{fam} families, {tt} TrueType")
    dash = int(got.get("Dash", "0"), 16)
    res.check("0x97 is an em dash at 100 pixels, as 3.1's ANSI table has it -- not a space's width",
              90 <= dash <= 110, f"{dash} px")
    res.check("SystemParametersInfo: SPI_GETBORDER is 3.1's 3, and 5 after SPI_SETBORDER for the run",
              (got.get("Border"), got.get("Border2")) == ("0003", "0005"), got)
    res.check("SPI_GETICONTITLELOGFONT: MS Sans Serif 8 point at 96 dpi, height -11",
              got.get("Title") == "FFF5", got)
    res.check("VerFindFile: a shared file no disk has goes in C:\\WINDOWS\\SYSTEM (18 bytes with its NUL)",
              (got.get("Ver"), got.get("VerDest")) == ("0000", "0012"), got)
    res.check("RectVisible: a rectangle on the bitmap is visible, one far off it is not",
              got.get("Vis") == "00010000", got)
    res.check("GetDoubleClickTime answers what SetDoubleClickTime(300) set",
              got.get("Dbl") == "012C", got)
    res.check("SwapMouseButton answers the buttons' state before each call: not swapped, then swapped",
              got.get("Swap") == "00000001", got)
    res.check("regions: PtInRegion in and out, OffsetRgn a SIMPLEREGION, GetRgnBox moved by it",
              got.get("Rgn") == "000100000002000F", got)
    res.check("GetSysColor(COLOR_BACKGROUND) is 3.1's light grey (VGA.DRV's OEMBIN), not teal",
              got.get("Desk") == "00C0C0C0", got)
    res.check("SetSysColors changes what GetSysColor answers for the run: COLOR_BTNFACE blue",
              got.get("Sys") == "00FF0000", got)
    res.check("CombineRgn XOR is exact: COMPLEXREGION, the overlap out, the rest in",
              got.get("Xor") == "000300000001", got)
    res.check("CreatePolygonRgn is the triangle, not its bounding box: 2,2 in, 15,15 out",
              got.get("Poly") == "00010000", got)
    res.check("ExcludeClipRect cuts a hole in the middle of the clip: a fill leaves it white",
              got.get("Hole") == "00FFFFFF00000000", got)
    text = dict(re.findall(r"(\w+)=(.*)", out))
    res.check("LZOpenFile finds a bare name in C:\\WINDOWS (OpenFile's search) and gives an LZ handle",
              got.get("LZ") == "0190", got)
    res.check("LZRead and LZSeek see the SZDD file's 10 expanded bytes, not its 24 on disk",
              (got.get("LZRead"), got.get("LZEnd"), text.get("LZText", "").strip()) ==
              ("000A", "0000000A", "Hello, w16"), text)
    res.check("GetExpandedName puts back the letter COMPRESS -r stored: LZPROBE.TX_ -> LZPROBE.TXT",
              text.get("LZName", "").strip().upper() == "C:\\WINDOWS\\LZPROBE.TXT", text)
    res.check("an LBS_EXTENDEDSEL list counts and lists its selection (Control Panel's Fonts)",
              got.get("Multi") == "0000000200010002", got)
    res.check("LB_GETSELCOUNT of a single-selection list is LB_ERR", got.get("Single") == "FFFF", got)
    res.check("an LBS_OWNERDRAWVARIABLE list keeps each item's own height (Fonts' Sample rows)",
              got.get("Var") == "001E001E", got)
    res.check("GetRasterizerCaps: TrueType available and enabled", got.get("Raster") == "0003", got)
    res.check("AddFontResource counts a TrueType file's font, and none in a file that is no font",
              got.get("Font") == "00010000", got)
    res.check("DOS's date and time are Windows' own to set: 2000-01-02 and 13:14 read back, the 30th of "
              "February refused", got.get("Clock") == "0007D00102FF000D0E", got)
    res.check("GetWindowPlacement: its length, shown normally, the window's own rectangle (Control Panel's "
              "exit)", got.get("Place") == "001600010064003C", got)
    res.check("CreateDC(\"DISPLAY\") gives the screen's DC (the Windows Tutorial's first call)",
              got.get("Disp") == "0001", got)
    cp = got.get("Cp", "")
    res.check("TA_UPDATECP: TextOut draws at the current position and moves it on by the text's width",
              len(cp) == 8 and cp[:4] == cp[4:] and cp[:4] != "0000", got)
    res.check("a WS_POPUP naming no border has none: its client is all 100 x 60",
              got.get("Pop") == "0064003C", got)
    res.check("DefWindowProc(WM_ERASEBKGND) with no class brush answers 0, so the program erases",
              got.get("Erase") == "0000", got)
    res.check("no WM_TIMER arrives after KillTimer, though the timer was due",
              got.get("Kill") == "0000", got)
    res.check("GetDIBits' 4-bit table is 3.1's sixteen: 7 dark grey, 15 white",
              got.get("Pal") == "0080808000FFFFFF", got)
    calc = got.get("Calc", "")
    res.check("DT_CALCRECT measures a wrapped line with the space it broke at",
              len(calc) == 8 and calc[:4] == calc[4:], got)
    res.check("ValidateRect takes a rectangle out of the update region: GetUpdateRect then answers what "
              "is left, and nothing once all of it is validated", got.get("Upd") == "00010000001E0064003C0000", got)
    res.check("LoadBitmap(NULL, OBM_REDUCE) with no VGA.DRV gives w16's own Minimize button, VGA.DRV's 19 x 18",
              got.get("Obm") == "00130012", got)
    res.check("WH_JOURNALPLAYBACK is played: its mouse move skipped past once, the pointer where it said",
              got.get("Jrnl") == "0001001E0028", got)
    res.check("DrawText underlines a line's last &-letter only, as 3.1 did: the b of \"&a&b\", not the a",
              got.get("Mark") == "FFFF0000", got)
    res.check("an Arial letter is smoothed by default: some of its pixels grey", int(got.get("Grey", "0"), 16) > 0, got)
    hard_truetype(dbg, res)
    fr = got.get("FileRes", "")
    res.check("GetFileResourceSize and GetFileResource read gdi.exe's bitmap from the file: a "
              "BITMAPINFOHEADER", len(fr) == 12 and int(fr[:8], 16) > 0x28 and fr[8:] == "0028", got)


def hard_truetype(dbg, res):
    """win16.conf's truetype=hard draws TrueType as 3.1 did: w16probe.exe's Arial "O" has no grey pixel."""
    was = sh(dbg, f"cat {WIN16_CONF}")
    sh(dbg, f"append {WIN16_CONF} truetype=hard")
    sh(dbg, "rm /home/Win16/WINDOWS/W16PROBE.INI")
    sh(dbg, "w16 /usr/share/w16/w16probe.exe")
    got = dict(re.findall(r"(\w+)=([0-9A-F]+)", sh(dbg, "cat /home/Win16/WINDOWS/W16PROBE.INI")))
    res.check("truetype=hard: an Arial letter's pixels are each black or white, none grey", got.get("Grey") == "0000", got)
    lines = [ln for ln in was.splitlines() if "=" in ln or ln.startswith("[")] if "No such" not in was else []
    if lines:                                    # the file as it was
        sh(dbg, f"write {WIN16_CONF} {lines[0]}")
        for ln in lines[1:]:
            sh(dbg, f"append {WIN16_CONF} {ln}")
    else:
        sh(dbg, f"rm {WIN16_CONF}")


def gdi_checks(dbg, qmp, res, tmp):
    sh(dbg, "mkdir /home/Win16")
    sh(dbg, "mkdir /home/Win16/WINDOWS")
    sh(dbg, f"write {INI} [GdiTest]")
    sh(dbg, f"append {INI} Square=1")
    sh(dbg, f"rm {GDI_CONF}")
    win = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", GDI_TITLE)
    res.check("gdi.exe opens its window", win is not None, "no window")
    if not win:
        return None
    ox, oy = win["x"] + FRAME, win["y"] + FRAME + CAPTION

    # The timer's square, or 5 s of waiting for it.
    im = None
    end = time.time() + 5
    while time.time() < end:
        im = shot(qmp, f"{tmp}/w16_gdi.png")
        if im.getpixel((ox + 103, oy + 53)) == GREEN:
            break
        time.sleep(0.3)
    px = lambda x, y: im.getpixel((ox + x, oy + y))
    res.check("the timer fired three times: the square is green", px(103, 53) == GREEN,
              f"got {px(103, 53)}")

    # The bitmap: its top half red, green, blue, white in 4-pixel bands,
    # its bottom half yellow, cyan, magenta, black.
    top, bottom = [RED, GREEN, BLUE, WHITE], [YELLOW, CYAN, MAGENTA, BLACK]
    def bitmap_at(x0, invert):
        got = [px(x0 + 4 * i + 1, 31) for i in range(4)] + [px(x0 + 4 * i + 1, 35) for i in range(4)]
        want = [inv(c) if invert else c for c in top + bottom]
        return got == want, f"got {got} want {want}"
    def bitmap_at_y(x0, y_top, y_bottom):
        got = [px(x0 + 4 * i + 1, y_top) for i in range(4)] + [px(x0 + 4 * i + 1, y_bottom) for i in range(4)]
        return got == top + bottom, f"got {got} want {top + bottom}"
    ok, why = bitmap_at(16, False)
    res.check("SetDIBitsToDevice: the bitmap's colours, top row first", ok, why)
    ok, why = bitmap_at(40, False)
    res.check("CreateDIBitmap + memory DC + BitBlt SRCCOPY: the same colours", ok, why)
    ok, why = bitmap_at(64, True)
    res.check("BitBlt NOTSRCCOPY: every colour inverted", ok, why)

    res.check("the red pen line runs x 16..79 and stops short of 80",
              px(16, 50) == RED and px(79, 50) == RED and px(80, 50) == WHITE,
              f"16:{px(16, 50)} 79:{px(79, 50)} 80:{px(80, 50)}")
    res.check("Rectangle: a black edge, a blue fill, nothing past its right edge",
              px(16, 56) == BLACK and px(47, 71) == BLACK and px(30, 64) == BLUE
              and px(48, 64) == WHITE,
              f"edge {px(16, 56)}/{px(47, 71)} fill {px(30, 64)} outside {px(48, 64)}")
    dark = lambda y: sum(1 for x in range(4, 120) if sum(px(x, y + 0)) < 200)
    res.check("the string-table string and the heaps' text are drawn",
              any(dark(y) for y in range(4, 16)) and any(dark(y) for y in range(80, 92)),
              "a text row has no dark pixel")
    res.check("GetPrivateProfileInt read Square=1 from the INI file: the square is magenta",
              px(123, 53) == MAGENTA, f"got {px(123, 53)}")
    # A metafile through MM_ANISOTROPIC at twice its size (PlayMetaFile, lib/uwmf),
    # SaveDC/IntersectClipRect/RestoreDC, and Polygon.
    res.check("PlayMetaFile: the red square at twice its size, a NULL pen leaving its right edge out",
              px(141, 31) == RED and px(178, 68) == RED and px(179, 50) == WHITE,
              f"141,31:{px(141, 31)} 178,68:{px(178, 68)} 179,50:{px(179, 50)}")
    res.check("PlayMetaFile: an ellipse, its middle green and its box's corner not",
              px(200, 50) == GREEN and px(182, 32) == WHITE and px(219, 50) != WHITE,
              f"middle {px(200, 50)} corner {px(182, 32)} right edge {px(219, 50)}")
    res.check("IntersectClipRect: the PatBlt is cut to the clip, inside SaveDC/RestoreDC",
              px(156, 84) == BLUE and px(144, 84) == WHITE and px(156, 96) == WHITE and px(166, 84) == WHITE,
              f"in {px(156, 84)} left {px(144, 84)} below {px(156, 96)} right {px(166, 84)}")
    res.check("Polygon: a blue triangle, its edge black, nothing past its long side",
              px(186, 86) == BLUE and px(180, 92) == BLACK and px(208, 104) == WHITE,
              f"inside {px(186, 86)} edge {px(180, 92)} outside {px(208, 104)}")
    res.check("a (200,100,50) brush at 256 colours: solid, as asked",
              all(px(x, y) == ODD for x in (100, 107, 115) for y in (62, 66, 69)),
              f"100,62:{px(100, 62)} 107,66:{px(107, 66)} 115,69:{px(115, 69)}")
    ok, why = bitmap_at_y(78, 59, 63)
    res.check("a RECORDED metafile played back: a red rectangle with a black edge, and the bitmap blitted into it",
              px(56, 60) == BLACK and px(75, 60) == BLACK and px(60, 60) == RED and px(66, 64) == RED and ok,
              f"edge {px(56, 60)}/{px(75, 60)} fill {px(60, 60)}/{px(66, 64)}; bitmap {why}")
    res.check("the program's caret: a 2-pixel black bar at its SetCaretPos, the white past it untouched",
              px(224, 80) == BLACK and px(225, 87) == BLACK and px(226, 80) == WHITE and px(224, 88) == WHITE,
              f"224,80:{px(224, 80)} 225,87:{px(225, 87)} 226,80:{px(226, 80)} 224,88:{px(224, 88)}")
    ini = sh(dbg, f"cat {INI}")
    res.check("WritePrivateProfileString kept the file's line and added its own",
              "Square=1" in ini and "Written=yes" in ini, f"the file: {ini!r}")
    return win


def window_controls(dbg, qmp, res, win):
    w0, h0 = win["w"], win["h"]
    # Each arrow's apex is the one black pixel in a row with button face either side and face above
    # or below (the frame's corner notch has black over and under): Minimize's is its bottom row,
    # Maximize's its top. On 3.11 they are four rows apart (rows 14 and 10).
    dbg.settle()
    im = shot(qmp, f"{TMP}/w16_caption.png")
    apex = sorted((x, y) for y in range(win["y"], win["y"] + 24)
                  for x in range(win["x"] + w0 - 48, win["x"] + w0 - 1)
                  if im.getpixel((x, y)) == BLACK and im.getpixel((x - 1, y)) == FACE
                  and im.getpixel((x + 1, y)) == FACE
                  and FACE in (im.getpixel((x, y - 1)), im.getpixel((x, y + 1))))
    res.check("Maximize's arrow sits a row higher than Minimize's, as on 3.11",
              len(apex) == 2 and apex[0][1] - apex[1][1] == 4, f"apexes {apex}")
    dbg.key("0x20", mods="alt")
    time.sleep(0.5)
    popups = [w for w in dbg.windows() if w["popup"]]
    res.check("Alt+Space opens w16's control menu", len(popups) == 1, f"popups={len(popups)}")
    dbg.key("x")
    end = time.time() + 5
    cur = win
    while time.time() < end:
        cur = dbg.window(GDI_TITLE)
        if cur and cur["state"] == "maximized" and cur["w"] > w0:
            break
        time.sleep(0.2)
    res.check("X maximizes the window", cur and cur["state"] == "maximized" and cur["w"] > w0,
              f"{cur and cur['state']} {cur and cur['w']}x{cur and cur['h']}")
    dbg.settle()
    im = shot(qmp, f"{TMP}/w16_max.png")
    res.check("maximized, the frame has no border: the caption reaches the top-left pixel",
              im.getpixel((cur["x"] + 2, cur["y"] + 2)) == FACE
              and im.getpixel((cur["x"] + 40, cur["y"] + 4)) == NAVY,
              f"corner {im.getpixel((cur['x'] + 2, cur['y'] + 2))} "
              f"caption {im.getpixel((cur['x'] + 40, cur['y'] + 4))}")
    dbg.key("0x20", mods="alt")
    time.sleep(0.5)
    dbg.key("0x0a")
    end = time.time() + 5
    while time.time() < end:
        cur = dbg.window(GDI_TITLE)
        if cur and cur["state"] == "normal" and (cur["w"], cur["h"]) == (w0, h0):
            break
        time.sleep(0.2)
    res.check("Alt+Space, Enter restores the size it had",
              cur and cur["state"] == "normal" and (cur["w"], cur["h"]) == (w0, h0),
              f"{cur and cur['state']} {cur and (cur['w'], cur['h'])} want {(w0, h0)}")
    dbg.close_window(GDI_TITLE)


def fault(dbg, res):
    win = dbg.spawn("/bin/w16 /usr/share/w16/fault.exe", FAULT_TITLE)
    res.check("fault.exe opens its window", win is not None, "no window")
    if not win:
        return
    before = set((dbg.send("sh ls /var/crash") or "").split())
    dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    end = time.time() + 5
    while time.time() < end and not any(w["popup"] for w in dbg.windows()):
        time.sleep(0.2)
    res.check("a click faults, and the Application Error box opens",
              any(w["popup"] for w in dbg.windows()), "no popup")
    dbg.key("0x0a")
    end = time.time() + 10
    while time.time() < end and dbg.window(FAULT_TITLE):
        time.sleep(0.3)
    res.check("Enter closes the box and the program ends", dbg.window(FAULT_TITLE) is None,
              "the window is still there")
    new = [f for f in (dbg.send("sh ls /var/crash") or "").split()
           if f.startswith("w16-") and f.endswith(".crash") and f not in before]
    out = dbg.send(f"sh crashlog {new[-1]}") if new else ""
    res.check("its crash report's message names the fault and where",
              "fault.exe: general protection fault at 0001:" in (out or ""),
              f"new reports {new}; crashlog said {(out or '')[:200]!r}")
    last = sh(dbg, f"cat {LAST.format('fault')}")
    res.check("its last run is recorded as the stop", "stopped" in last and "general protection" in last,
              f"the file: {last!r}")


def settings(dbg, qmp, res):
    # scale=2 for gdi.exe alone: its window at twice the 1x size it opened
    # at above (240x150 asked; the 3.1 frame is drawn doubled too).
    sh(dbg, f"write {GDI_CONF} scale=2")
    win = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", GDI_TITLE)
    res.check("scale=2 in a program's own settings doubles its window",
              win is not None and (win["w"], win["h"]) == (480, 300),
              f"got {win and (win['w'], win['h'])}, want (480, 300)")
    dbg.close_window(GDI_TITLE)
    # 1.5x: the window is exactly 1.5 times the size, and the METHOD is
    # what spreads a pixel: across the magenta square's left edge (client
    # x 120, 1x window x 124, so 186 at 1.5x) Pixels leaves only magenta
    # and white, and Smooth blends them.
    strips = {}
    for method in ("pixels", "smooth"):
        sh(dbg, f"write {GDI_CONF} scale=1.5")
        sh(dbg, f"append {GDI_CONF} method={method}")
        win = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", GDI_TITLE)
        if not win:
            res.check(f"scale=1.5 method={method} opens gdi.exe", False, "no window")
            continue
        if method == "pixels":
            res.check("scale=1.5 makes the window exactly 1.5 times its size",
                      (win["w"], win["h"]) == (360, 225), f"got {(win['w'], win['h'])}, want (360, 225)")
        time.sleep(1.5)                          # the timer's square, as above
        im = shot(qmp, f"{TMP}/w16_scale_{method}.png")
        y = win["y"] + (24 + 54) * 3 // 2
        strips[method] = {im.getpixel((win["x"] + x, y)) for x in range(180, 196)}
        dbg.close_window(GDI_TITLE)
    pure = {MAGENTA, WHITE}
    res.check("Pixels spreads a pixel hard: only magenta and white across the edge",
              strips.get("pixels") is not None and strips["pixels"] <= pure and MAGENTA in strips["pixels"],
              f"colours {strips.get('pixels')}")
    res.check("Smooth blends them across the edge",
              strips.get("smooth") is not None and bool(strips["smooth"] - pure),
              f"colours {strips.get('smooth')}")
    sh(dbg, f"rm {GDI_CONF}")
    # frame=desktop for hello.exe: toywm draws a frame (the content sits
    # inside the window), and the client is hello's 400x200 less the 3.1
    # frame it was sized for (2*4 by 2*4 + 19).
    sh(dbg, f"write {HELLO_CONF} frame=desktop")
    win = dbg.spawn("/bin/w16 /usr/share/w16/hello.exe", HELLO_TITLE)
    c = win and win["content"]
    res.check("frame=desktop gives toywm's frame, the client the size the program asked for",
              bool(win) and c["y"] > win["y"] and (c["w"], c["h"]) == (400 - 8, 200 - 27),
              f"window {win and (win['x'], win['y'], win['w'], win['h'])} content {c}")
    dbg.close_window(HELLO_TITLE)
    sh(dbg, f"rm {HELLO_CONF}")


# A look of scanlines alone, in ucrt.h's keys: FLAT, because a curve resamples the rows and the
# line under a row drifts between the two screen lines across the client -- nothing to average.
CASCADE = ("Subtle", "Soft CRT", "Classic CRT", "Curved")   # the presets the control menu lists after Off
SCANLINES = ("effect=on", "crt_scanlines=high", "crt_glow=off", "crt_vignette=off",
             "crt_curve=off", "crt_mask=off")


def screen_effect(dbg, qmp, res):
    """The CRT effect over a program's client area (w16.c's draw_effect): gdi.exe at 2x, drawn
    plain and then with strong scanlines alone. The caption is the control -- the same pixels both times --
    and in the client a scanline closes each 3.1 row, so at 2x the second screen line of every
    row is darker than its first; plain, the two are the same. Then the control menu's Screen
    effect cascade, by keys: Alt+Space, E opens it beside the menu, O is Off -- saved to the
    program's own file by hlpbroker, as the sandboxed w16 may not write it -- and the window is
    plain again."""
    def timer_square(win):
        # The square its timer draws after three ticks (client 100-107, 50-57 in gdi.asm, so 1x window 104-111, 74-81):
        # pure green plain, green-ish under the effect. The last thing gdi.exe draws.
        def seen():
            im = shot(qmp, f"{TMP}/w16_fx_wait.png")
            r, g, b = im.getpixel((win["x"] + 2 * 107, win["y"] + 2 * 77))
            return im if g > 140 and r < 100 and b < 100 else None
        return poll(seen, 8, step=0.5)

    def caption(im, win):
        # The whole caption band, every row: a scanline darkens only some of them.
        return [im.getpixel((win["x"] + x, win["y"] + y)) for y in range(2, 2 * 19)
                for x in range(30, win["w"] - 60, 3)]

    def line_contrast(im, win):
        # Mean luminance of each row's first screen line minus its second, over the middle of
        # the client.
        x0, x1 = win["x"] + win["w"] // 4, win["x"] + 3 * win["w"] // 4
        y0 = win["y"] + 2 * (4 + 19) + 20
        y1 = win["y"] + win["h"] - 40
        first = second = n = 0
        for y in range(y0 - (y0 - win["y"]) % 2, y1 - 1, 2):
            for x in range(x0, x1, 4):
                first += sum(im.getpixel((x, y)))
                second += sum(im.getpixel((x, y + 1)))
                n += 1
        return (first - second) / (3 * n) if n else 0

    # No warp_cursor to park the pointer: a warped real pointer outlives this check, and the
    # next one's injected click then releases where it is (launcher()'s calls button went red).
    sh(dbg, f"write {GDI_CONF} scale=2")
    win = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", GDI_TITLE)
    plain = win and timer_square(win)
    if not plain:
        res.check("screen effect: gdi.exe opens at 2x and draws its square", False, f"window {win}")
        dbg.close_window(GDI_TITLE)
        sh(dbg, f"rm {GDI_CONF}")
        return
    dbg.close_window(GDI_TITLE)
    for line in SCANLINES:
        sh(dbg, f"append {GDI_CONF} {line}")
    win = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", GDI_TITLE)
    fx = win and timer_square(win)
    if not fx:
        res.check("screen effect: gdi.exe opens with scanlines on", False, f"window {win}")
        dbg.close_window(GDI_TITLE)
        sh(dbg, f"rm {GDI_CONF}")
        return
    res.check("screen effect: the 3.1 caption is drawn as without it", caption(fx, win) == caption(plain, win),
              "caption pixels differ")
    cp, cf = line_contrast(plain, win), line_contrast(fx, win)
    res.check("screen effect: a scanline per 3.1 row -- each row's second line darker, plain not",
              abs(cp) < 1 and cf > 8, f"first-minus-second line: plain {cp:.1f}, scanlines {cf:.1f}")
    # The cascade.
    dbg.key("0x20", mods="alt")
    dbg.settle()
    dbg.key("e")
    opened = poll(lambda: len([w for w in dbg.windows() if w["popup"]]) == 2, 3)
    res.check("Alt+Space, E opens the Screen effect cascade beside the control menu", opened,
              f"popups {len([w for w in dbg.windows() if w['popup']])}")
    # The pointer over its last row lights that row AT ONCE: only the cascade's lit row changes,
    # and w16 once redrew for the top menu's alone, so the light trailed the pointer.
    pops = sorted((w for w in dbg.windows() if w["popup"]), key=lambda w: w["x"])
    if len(pops) == 2:
        c = pops[1]
        n = 1 + len(CASCADE)                     # Off, then the presets
        row = (c["h"] // 2 - 3) // n             # 1x: a border row, the rows, the border, a shadow
        mid = lambda k: c["y"] + 2 * (1 + k * row) + row        # row k's middle, at 2x
        dbg.move(c["x"] + c["w"] // 2, mid(n - 1))
        dbg.settle()
        im = shot(qmp, f"{TMP}/w16_fx_cascade.png")
        lit = [im.getpixel((c["x"] + 8, mid(k))) == NAVY for k in range(n)]   # menu16.c's lit row, at VGA
        res.check("the pointer over the cascade's last row lights it, and only it, at once",
                  lit == [False] * (n - 1) + [True], f"lit rows {lit}")
    # Esc, Esc: the menus close and the window is drawn again with its client unchanged -- the
    # kept result, not the passes run again (w16.c's g_fx_out). It must still be the effect.
    dbg.key("0x1b")
    dbg.key("0x1b")
    dbg.settle()
    kept = line_contrast(shot(qmp, f"{TMP}/w16_fx_kept.png"), win)
    res.check("with the menus closed, the kept effect is drawn: the scanlines are still there",
              kept > 8, f"first-minus-second line {kept:.1f}")
    dbg.key("0x20", mods="alt")
    dbg.settle()
    dbg.key("e")
    poll(lambda: len([w for w in dbg.windows() if w["popup"]]) == 2, 3)
    dbg.key("o")
    saved = poll(lambda: "effect=off" in sh(dbg, f"cat {GDI_CONF}"), 5)
    res.check("O chooses Off, saved to the program's own settings (by hlpbroker)", saved,
              sh(dbg, f"cat {GDI_CONF}").strip().replace("\n", " "))
    again = poll(lambda: abs(line_contrast(shot(qmp, f"{TMP}/w16_fx_off.png"), win)) < 1, 5)
    res.check("...and the window is drawn plain at once", bool(again), "scanlines still there")
    # The look is its own (no preset is scanlines alone), so after Off the cascade offers Custom,
    # and T turns that look back on -- Off is not a one-way trip.
    dbg.key("0x20", mods="alt")
    dbg.settle()
    dbg.key("e")
    poll(lambda: len([w for w in dbg.windows() if w["popup"]]) == 2, 3)
    dbg.key("t")
    on = poll(lambda: "effect=on" in sh(dbg, f"cat {GDI_CONF}"), 5)
    conf = sh(dbg, f"cat {GDI_CONF}")
    res.check("after Off, the cascade's Custom (T) turns the program's own look back on, unchanged",
              bool(on) and "crt_scanlines=high" in conf and "crt_glow=off" in conf,
              conf.strip().replace("\n", " "))
    dbg.close_window(GDI_TITLE)
    sh(dbg, f"rm {GDI_CONF}")


def launcher(dbg, res):
    win = dbg.spawn("/bin/wm/apps/retrolauncher", "Retro Launcher")
    res.check("Retro Launcher opens", win is not None, "no window")
    if not win:
        return
    time.sleep(3)                                 # three programs, checked a few a tick
    # The list is by name now: narrow it to the Hello program (a window of its own), as a person would.
    search = dbg.widgets("Retro Launcher").get("search")
    if search:
        dbg.click(search["screen"]["x"] + 40, search["screen"]["y"] + 10)
        for ch in "hello":
            dbg.send(f"gui key {ch}")
            dbg.settle(0.1)
    grid = dbg.widgets("Retro Launcher").get("programs")
    res.check("its program grid is there", grid is not None, "no 'programs' widget")
    if grid:
        g = grid["screen"]
        before = {w["title"] for w in dbg.windows()}
        x, y = g["x"] + 40, g["y"] + 30
        dbg.click(x, y, settle=False)
        dbg.click(x, y)
        end = time.time() + 10
        new = set()
        while time.time() < end and not any("(Win16)" in t for t in new):
            new = {w["title"] for w in dbg.windows()} - before - {"Retro Launcher"}
            time.sleep(0.3)
        res.check("a double click on a program's tile runs it", any("(Win16)" in t for t in new),
                  f"new windows {new}")
        for t in new:
            dbg.close_window(t)
        # The selected program's "missing calls" button opens its window.
        # Polled: the launcher republishes its widgets when it redraws on
        # getting the focus back from the program just closed.
        btn, end = None, time.time() + 5
        while not btn and time.time() < end:
            btn = dbg.widgets("Retro Launcher").get("calls")
            time.sleep(0.3)
        if btn:
            dbg.click(btn["screen"]["x"] + 10, btn["screen"]["y"] + 8)
        end = time.time() + 10
        cw = None
        while time.time() < end and not cw:
            cw = next((w["title"] for w in dbg.windows() if w["title"].endswith(" - calls")), None)
            time.sleep(0.3)
        res.check("the details panel's button opens the program's calls window", cw is not None,
                  f"button {btn and btn['screen']}, windows {[w['title'] for w in dbg.windows()]}")
        if cw:
            dbg.close_window(cw)
    dbg.close_window("Retro Launcher")
    dbg.send("gui spawn /bin/wm/apps/retrolauncher /usr/share/w16/hello.exe")
    end = time.time() + 10
    while time.time() < end and not dbg.window(HELLO_TITLE):
        time.sleep(0.3)
    res.check("`retrolauncher FILE` runs the program, with no launcher window",
              dbg.window(HELLO_TITLE) is not None and dbg.window("Retro Launcher") is None,
              f"windows {[w['title'] for w in dbg.windows()]}")
    dbg.close_window(HELLO_TITLE)
    last = sh(dbg, f"cat {LAST.format('hello')}")
    res.check("closing it records the run as ended, before the window goes", " ended" in last,
              f"the file: {last!r}")


def capture_cursor(dbg, qmp, res):
    """A scroll bar's drag holds the capture, and under a capture the
    pointer keeps the shape it had -- an arrow from the bar, even over
    the edit's text, where it was an I-beam (Notepad, 2026-10-20)."""
    win = dbg.spawn("/bin/w16 /usr/share/w16/hello.exe", HELLO_TITLE)
    res.check("hello.exe opens, with its scrolling edit", win is not None, "no window")
    if not win:
        return
    ARROW, IBEAM = 0, 4                     # wm_internal.h's WM_CURSOR_NORMAL, WM_CURSOR_TEXT
    ox, oy = win["x"] + FRAME, win["y"] + FRAME + CAPTION
    bar_x, thumb_y = ox + 16 + 200 - 9, oy + 40 + 1 + 17 + 8   # the edit's right edge; under its up arrow
    text = (ox + 60, oy + 70)
    dbg.warp_cursor(qmp, *text)
    dbg.settle()
    res.check("over the edit's text the pointer is the I-beam", dbg.cursor_shape() == IBEAM,
              f"shape {dbg.cursor_shape()}")
    # The wheel, which 3.1 never had: a notch down scrolls the edit under the pointer (WM_VSCROLL),
    # a notch up puts back exactly what was there. The pointer stays put, so its sprite is in all three.
    box = (win["x"], win["y"], win["x"] + win["w"], win["y"] + win["h"])
    before = shot(qmp, f"{TMP}/w16_wheel0.png").crop(box)
    dbg.wheel(-1)
    dbg.settle(0.5)
    down = shot(qmp, f"{TMP}/w16_wheel1.png").crop(box)
    dbg.wheel(1)
    dbg.settle(0.5)
    up = shot(qmp, f"{TMP}/w16_wheel2.png").crop(box)
    res.check("a wheel notch scrolls the edit under the pointer, and one back restores it",
              down.tobytes() != before.tobytes() and up.tobytes() == before.tobytes(),
              f"changed {down.tobytes() != before.tobytes()}, restored {up.tobytes() == before.tobytes()}")
    dbg.warp_cursor(qmp, bar_x, thumb_y)
    dbg.settle()
    res.check("over its scroll bar, the arrow", dbg.cursor_shape() == ARROW, f"shape {dbg.cursor_shape()}")
    qmp.mouse_down()
    dbg.settle()
    for i in range(1, 5):
        dbg.warp_cursor(qmp, bar_x + (text[0] - bar_x) * i // 4, thumb_y + (text[1] - thumb_y) * i // 4)
    dbg.settle()
    held = dbg.cursor_shape()
    qmp.mouse_up()
    dbg.settle()
    res.check("dragging the thumb onto the text keeps the arrow", held == ARROW, f"shape {held}")
    dbg.warp_cursor(qmp, text[0] + 10, text[1])
    dbg.settle()
    res.check("...and once let go, the text's I-beam again", dbg.cursor_shape() == IBEAM,
              f"shape {dbg.cursor_shape()}")
    dbg.close_window(HELLO_TITLE)


WIN16_CONF = "/etc/win16.conf"


def layout_lines(dbg, prefix, ok, timeout=6.0):
    """The app's layout log lines until `ok` says they are enough."""
    acc, end = [], time.time() + timeout
    while time.time() < end:
        acc += dbg.logs(prefix + ":", clear=True)
        if ok(acc):
            break
        time.sleep(0.2)
    return acc


def layout_int(lines, name):
    got = None
    for ln in lines:
        m = re.search(r": layout " + re.escape(name) + r" (-?\d+)$", ln)
        if m:
            got = int(m.group(1))
    return got


def drives_and_folders(dbg, res):
    """Drive D: reaches its folder; the folder list has no cap; a folder's
    right-click Move down is saved in order."""
    # hello.exe writes D:\HELLO.TXT: with D: mapped, the file lands there.
    sh(dbg, "mkdir /home/w16d")
    sh(dbg, f"write {WIN16_CONF} drive_d=/home/w16d")
    if dbg.spawn("/bin/w16 /usr/share/w16/hello.exe", HELLO_TITLE):
        dbg.close_window(HELLO_TITLE)
    listing = sh(dbg, "ls /home/w16d")
    res.check("drive D: is its folder: hello.exe's D:\\HELLO.TXT lands in /home/w16d",
              "hello.txt" in listing.lower(), f"ls: {listing!r}")
    # Nine folders, past the eight there used to be room for.
    sh(dbg, f"write {WIN16_CONF} folders=9")
    sh(dbg, f"append {WIN16_CONF} folder1=/usr/share/w16")
    sh(dbg, f"append {WIN16_CONF} folder2=/home/Win16")
    for i in range(3, 10):
        sh(dbg, f"mkdir /home/w16f{i}")
        sh(dbg, f"append {WIN16_CONF} folder{i}=/home/w16f{i}")
    dbg.logs("retrolauncher:", clear=True)
    win = dbg.spawn("/bin/wm/apps/retrolauncher", "Retro Launcher")
    res.check("Retro Launcher opens on the nine folders", win is not None, "no window")
    if not win:
        return
    lines = layout_lines(dbg, "retrolauncher", lambda a: layout_int(a, "folders.rows") is not None)
    rows = layout_int(lines, "folders.rows")
    res.check("all nine folders are listed: ten sidebar rows with All programs", rows == 10, f"rows {rows}")
    row_h = layout_int(lines, "folders.row_h") or 0
    side = dbg.widgets("Retro Launcher").get("folders")
    if not side or not row_h:
        res.check("the sidebar reports where it is", False, f"{side} row_h {row_h}")
    else:
        # Right-click Samples (row 1), then Move down (the menu's fifth row:
        # Change, Rename, a line, Move up, Move down).
        sc = side["screen"]
        dbg.logs("retrolauncher:", clear=True)
        dbg.rclick(sc["x"] + 20, sc["y"] + row_h + row_h // 2)
        lines = layout_lines(dbg, "retrolauncher", lambda a: any("ctxmenu.item 0 4 " in ln for ln in a))
        item = None
        for ln in lines:
            m = re.search(r"ctxmenu\.item 0 4 (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
            if m:
                item = tuple(int(v) for v in m.groups())
        res.check("a right-click on a folder opens its menu", item is not None, f"{len(lines)} layout lines")
        if item:
            c = dbg.window("Retro Launcher")["content"]
            dbg.click(c["x"] + item[0] + item[2] // 2, c["y"] + item[1] + item[3] // 2)
            dbg.settle()
            conf = sh(dbg, f"cat {WIN16_CONF}")
            res.check("Move down is saved: /home/Win16 first, then the samples",
                      "folder1=/home/Win16" in conf and "folder2=/usr/share/w16" in conf, f"the file: {conf!r}")
    dbg.close_window("Retro Launcher")
    sh(dbg, f"rm {WIN16_CONF}")


LAUNCHER_BIN = "/bin/wm/apps/retrolauncher"


def wait_title(dbg, title, t=10.0):
    end = time.time() + t
    while time.time() < end:
        if dbg.window(title):
            return True
        time.sleep(0.3)
    return False


def from_file_manager(dbg, qmp, res):
    """What the File Manager's Open does with an .exe (`retrolauncher FILE`), the
    rows it shows about one (`--details`), and its right-click actions."""
    sh(dbg, "write /home/notwin.exe MZ this is not a Windows program")
    out = sh(dbg, f"{LAUNCHER_BIN} --details /usr/share/w16/fault.exe")
    res.check("--details: fault.exe runs partly, one call missing", "Runs\tPartly: 1 call missing" in out, repr(out))
    out = sh(dbg, f"{LAUNCHER_BIN} --details /home/notwin.exe")
    res.check("--details: an MZ file that is not Windows 3.x is named for what it is",
              "Program\ta DOS program" in out and "Runs\tNo" in out, repr(out))
    # Opened from elsewhere, by the default: ask only when it may not work.
    dbg.send(f"gui spawn {LAUNCHER_BIN} /usr/share/w16/hello.exe")
    res.check("hello.exe, which w16 has every call of, runs at once", wait_title(dbg, HELLO_TITLE),
              f"windows {[w['title'] for w in dbg.windows()]}")
    dbg.close_window(HELLO_TITLE)
    dbg.send(f"gui spawn {LAUNCHER_BIN} /usr/share/w16/fault.exe")
    res.check("fault.exe, which lacks a call, gets the run card first", wait_title(dbg, "Run fault.exe?"),
              f"windows {[w['title'] for w in dbg.windows()]}")
    dbg.close_window("Run fault.exe?")
    dbg.send(f"gui spawn {LAUNCHER_BIN} /home/notwin.exe")
    res.check("a DOS .exe gets the card that says it cannot run here", wait_title(dbg, "notwin.exe cannot run here"),
              f"windows {[w['title'] for w in dbg.windows()]}")
    dbg.close_window("notwin.exe cannot run here")
    sh(dbg, f"write {WIN16_CONF} from_files=ask")
    dbg.send(f"gui spawn {LAUNCHER_BIN} /usr/share/w16/hello.exe")
    res.check("from_files=ask: even hello.exe gets the card", wait_title(dbg, "Run hello.exe?"),
              f"windows {[w['title'] for w in dbg.windows()]}")
    dbg.close_window("Run hello.exe?")
    sh(dbg, f"rm {WIN16_CONF}")
    # The File Manager's right-click menu on an .exe: the launcher's own
    # actions under Open (its desktop entry's [Desktop Action]s).
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} /usr/share/w16", fm.TITLE)
    res.check("the File Manager opens on the samples", win is not None, "no window")
    if not win:
        return
    lay = fm.wait_layout(dbg, win, lambda l: l.rowy.get(0) is not None and l.rowh)
    if not lay:
        res.check("the File Manager reports its rows", False, "no row geometry")
        dbg.close_window(fm.TITLE)
        return
    px, py, pw, _ = lay.pane[0]
    for ch in "fault.exe":
        dbg.key("0x2e" if ch == "." else ch)
    fm.wait_layout(dbg, win, lambda l: l.selected == "fault.exe")
    # Its row from the listing, never a fixed one: a new sample sorting before it moves it down.
    names = sorted(n for n in fm.listing(dbg, "/usr/share/w16") if not n.endswith("/"))
    row = 1 + names.index("fault.exe") if "fault.exe" in names else 2   # under ".."
    mark = fm.ctx_mark(dbg)
    fm.sure_rclick(dbg, qmp, lay.ox + px + pw // 2, lay.oy + lay.rowy[0] + lay.rowh * row + lay.rowh // 2)
    rows = fm.ctx_rows(dbg, mark) or []
    res.check("its right-click menu has Show its calls and Show in Retro Launcher under Open",
              "Show its calls" in rows and "Show in Retro Launcher" in rows
              and rows.index("Show its calls") == rows.index("Open") + 1, f"rows {rows}")
    if "Show its calls" in rows:
        fm.ctx_pick(dbg, qmp, win, rows, "Show its calls")
        res.check("...and Show its calls opens its calls window", wait_title(dbg, "Fault test - calls"),
                  f"windows {[w['title'] for w in dbg.windows()]}")
        dbg.close_window("Fault test - calls")
    dbg.close_window(fm.TITLE)
    sh(dbg, f"rm {fm.FILES_CONF}")


def layout_rect(lines, name):
    got = None
    for ln in lines:
        m = re.search(r": layout " + re.escape(name) + r" (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
        if m:
            got = tuple(int(v) for v in m.groups())
    return got


def panel_and_options(dbg, qmp, res):
    """The details panel scrolls above a pinned footer, and the Options
    window resizes, its Drives list growing with it and a card under it."""
    dbg.send(f"gui spawn {LAUNCHER_BIN} --show /usr/share/w16/fault.exe")
    res.check("--show opens Retro Launcher", wait_title(dbg, "Retro Launcher"),
              f"windows {[w['title'] for w in dbg.windows()]}")
    dbg.send("gui resize 1000 380")
    dbg.settle()
    # POLLED ON THE ASSERTION, not read once after a second: the launcher
    # reports no widgets at all until its window thread has laid out the
    # resized window, and at start-up that thread is walking the drives --
    # `widgets []` 17 runs in 46 under the parallel suite.
    short = lambda w: w if "run" in w and "drive-c" not in w else None
    w = poll(lambda: short(dbg.widgets("Retro Launcher")), 10) or dbg.widgets("Retro Launcher")
    res.check("a short window: fault.exe's Run is in the footer, its Drive C: row scrolled out of view",
              "run" in w and "drive-c" not in w, f"widgets {sorted(w)}")
    det = w.get("details")
    if det:
        dbg.warp_cursor(qmp, *dbg.widget_center("details", "Retro Launcher"))
        dbg.wheel(-10)
        above = lambda w: (w if w.get("drive-c") and w.get("run") and
                           w["drive-c"]["y"] + w["drive-c"]["h"] <= w["run"]["y"] else None)
        w = poll(lambda: above(dbg.widgets("Retro Launcher")), 5) or dbg.widgets("Retro Launcher")
        dc, run = w.get("drive-c"), w.get("run")
        res.check("the wheel over the panel scrolls Drive C: into view, above the pinned Run",
                  dc is not None and run is not None and dc["y"] + dc["h"] <= run["y"],
                  f"drive-c {dc} run {run}")
    else:
        res.check("the panel reports its scrolling area", False, f"widgets {sorted(w)}")
    # Options, from its toolbar button (the toolbar's tenth slot, after the view buttons), once
    # a resize has made the window wide enough for all of it and repaint.
    dbg.logs("retrolauncher:", clear=True)
    dbg.send("gui resize 1200 380")
    lines = layout_lines(dbg, "retrolauncher", lambda a: layout_rect(a, "toolbar.button 9") is not None)
    b = layout_rect(lines, "toolbar.button 9")
    win = dbg.window("Retro Launcher")
    if not b or not win:
        res.check("the toolbar reports its Options button", False, f"{b} {win}")
        dbg.close_window("Retro Launcher")
        return
    c = win["content"]
    dbg.click(c["x"] + b[0] + b[2] // 2, c["y"] + b[1] + b[3] // 2)
    res.check("Options... opens the Options window", wait_title(dbg, "Retro Launcher Options"),
              f"windows {[x['title'] for x in dbg.windows()]}")
    before = dbg.window("Retro Launcher Options")
    if before:
        dbg.send(f"gui resize {before['content']['w'] + 120} {before['content']['h'] + 80}")
        time.sleep(1.5)
        after = dbg.window("Retro Launcher Options")
        res.check("the Options window takes a bigger size",
                  after and after["content"]["w"] == before["content"]["w"] + 120,
                  f"before {before['content']} after {after and after['content']}")
        dbg.send("gui resize 200 150")
        time.sleep(1.5)
        small = dbg.window("Retro Launcher Options")
        res.check("...and a smaller one, its page scrolling, but not as small as 200x150",
                  small and small["content"]["h"] < before["content"]["h"]
                  and small["content"]["w"] > 200 and small["content"]["h"] > 150,
                  f"opened {before['content']} asked 200x150, got {small and small['content']}")
        dbg.close_window("Retro Launcher Options")
    dbg.close_window("Retro Launcher")


def sound_page(dbg, res):
    """Options > Sound: a row per event (Windows 3.11's seven, with no
    WIN.INI on drive C:), each Play cell playing SaunaOS's sound for it --
    and none once the Sounds choice is Off, before OK."""
    def opt(key):
        for line in reversed(dbg.logs(f"retrolauncher options: layout {key} ", clear=False)):
            try:
                return [int(v) for v in line.split(f"layout {key} ", 1)[1].split()[:4]]
            except ValueError:
                return None
        return None

    dbg.logs("retrolauncher", clear=True)
    dbg.send(f"gui spawn {LAUNCHER_BIN} --show /usr/share/w16/hello.exe")
    if not wait_title(dbg, "Retro Launcher"):
        res.check("Retro Launcher opens for the Sound page", False, "")
        return
    dbg.send("gui resize 1200 600")
    lines = layout_lines(dbg, "retrolauncher", lambda a: layout_rect(a, "toolbar.button 9") is not None)
    b = layout_rect(lines, "toolbar.button 9")
    win = dbg.window("Retro Launcher")
    if not b or not win:
        res.check("the toolbar reports its Options button", False, f"{b} {win}")
        dbg.close_window("Retro Launcher")
        return
    dbg.click(win["content"]["x"] + b[0] + b[2] // 2, win["content"]["y"] + b[1] + b[3] // 2)
    end = time.time() + 8
    while time.time() < end and not opt("pages"):
        time.sleep(0.3)
    ow = dbg.window("Retro Launcher Options")
    pg, rh = opt("pages"), opt("pages.row_h")
    if not ow or not pg or not rh:
        res.check("Options opens, its pages reported", False, f"{ow} {pg} {rh}")
        dbg.close_window("Retro Launcher")
        return
    oc = ow["content"]
    dbg.click(oc["x"] + pg[0] + 30, oc["y"] + pg[1] + 3 * rh[0] + rh[0] // 2)   # Sound: General, Display, Screen effect, Sound
    dbg.settle(0.6)
    shown, play, rowh, head = opt("sound-table.shown"), opt("sound-table.col 4"), opt("sound-table.row_h"), \
        opt("sound-table.header_h")
    res.check("Options > Sound lists Windows 3.11's seven events (drive C: has no WIN.INI)",
              bool(shown) and shown[0] == 7, f"shown {shown}")
    if not (play and rowh and head):
        res.check("the sound table reports its Play column", False, f"{play} {rowh} {head}")
    else:
        y = oc["y"] + play[1] + head[0] + rowh[0] // 2
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(oc["x"] + play[0] + play[2] // 3, y)
        dbg.settle(0.4)
        got = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": play " in ln]
        res.check("the first row's Play plays SaunaOS's chime for the Default Beep",
                  any(ln.endswith("play 0 /usr/share/sounds/chime.wav") for ln in got), f"{got}")
        off = opt("sounds.slot 2")
        if off:
            dbg.click(oc["x"] + off[0] + off[2] // 2, oc["y"] + off[1] + off[3] // 2)
            dbg.settle(0.4)
            dbg.click(oc["x"] + play[0] + play[2] // 3, y)
            dbg.settle(0.4)
            got = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": play " in ln]
            res.check("with Sounds Off chosen (not yet OK), Play plays nothing", not got, f"{got}")
        else:
            res.check("the Sounds choice reports its slots", False, "")
    dbg.close_window("Retro Launcher Options")
    dbg.close_window("Retro Launcher")


DLG_TITLE = "Dialog test (Win16)"
DLG_INI = "/home/Win16/WINDOWS/DIALOG.INI"


def wait_titles(dbg, pred, t=8.0):
    end = time.time() + t
    while time.time() < end:
        titles = [w["title"] for w in dbg.windows()]
        if pred(titles):
            return titles
        time.sleep(0.3)
    return [w["title"] for w in dbg.windows()]


def key_repeat(dbg, qmp, res):
    """A held key repeats on WIN.INI's KeyboardDelay and KeyboardSpeed, not SaunaOS's: with a second's
    delay and two a second (3 and 0), a physical 1 held 1.75 s is typed at 0, 1.0 and 1.5 s -- Value=111 --
    where QEMU's own PS/2 typematic, which w16 must drop, would type a dozen. A key held through QMP,
    since only a physical press is ever repeated (input16.c)."""
    sh(dbg, "write /home/Win16/WINDOWS/WIN.INI [windows]")
    sh(dbg, "append /home/Win16/WINDOWS/WIN.INI KeyboardDelay=3")
    sh(dbg, "append /home/Win16/WINDOWS/WIN.INI KeyboardSpeed=0")
    dbg.send("gui key 0xF789")                    # F2: the Number dialog, 42 selected
    titles = wait_titles(dbg, lambda t: "Number" in t)
    if "Number" not in titles:
        res.check("the Number dialog opens again for the repeat check", False, f"windows {titles}")
        return
    qmp.key_down("1")
    time.sleep(1.75)
    qmp.key_up("1")
    dbg.settle(0.5)
    dbg.send("gui key 0x0a")
    value, end = None, time.time() + 6
    while value != "111" and time.time() < end:
        m = re.search(r"^Value=(\d+)", sh(dbg, f"cat {DLG_INI}"), re.M)
        value = m.group(1) if m else None
        time.sleep(0.3)
    res.check("a held key repeats on WIN.INI's KeyboardDelay and KeyboardSpeed: Value=111", value == "111",
              f"Value={value}")
    wait_titles(dbg, lambda t: "Number" not in t)
    sh(dbg, "rm /home/Win16/WINDOWS/WIN.INI")


def dialog_control_menu(dbg, res, title):
    """A dialog's control-menu box (3.11's Move and Close): a press opens the menu, a press outside
    shuts it, and a double click on the box closes the dialog, as Cancel."""
    win = dbg.window(title)
    if not win:
        res.check("the dialog's control-menu box can be found", False, f"no window {title!r}")
        return
    box = (win["x"] + 12, win["y"] + 10)          # inside the box at the caption's left, at 1x or 2x
    dbg.click(*box)
    popups = [w for w in dbg.windows() if w.get("popup")]
    res.check("a press on a dialog's control-menu box opens its menu", len(popups) == 1,
              f"popups {len(popups)}")
    dbg.click(win["x"] + win["w"] - 20, win["y"] + win["h"] - 10)
    dbg.send(f"gui click {box[0]} {box[1]}")
    dbg.send(f"gui click {box[0]} {box[1]}")
    titles = wait_titles(dbg, lambda t: title not in t)
    res.check("...and a double click on the box closes the dialog", title not in titles, f"windows {titles}")


def ini_has(dbg, line, t=6.0):
    end = time.time() + t
    while time.time() < end:
        if line in sh(dbg, f"cat {DLG_INI}"):
            return True
        time.sleep(0.3)
    return False


def owner_drawn_list(dbg, qmp, res, title):
    """The Number dialog's owner-drawn list (LBS_OWNERDRAWFIXED): dialog.exe measures its rows 20 tall and
    fills them dark grey, the selected (middle) one black -- read down the list's middle as bands."""
    win = title and dbg.window(title)
    if not win:
        res.check("the owner-drawn list can be read", False, f"no window {title!r}")
        return
    im = shot(qmp, f"{TMP}/w16_ownerdraw.png")
    c = win["content"]
    dark = (0x40, 0x40, 0x40)
    # The list's top row: the first with a long run of the fill (anti-aliased text has the odd pixel).
    at = None
    for y in range(c["y"], c["y"] + c["h"]):
        xs = [x for x in range(c["x"], c["x"] + c["w"]) if im.getpixel((x, y)) == dark]
        if len(xs) >= 40:
            at = (xs[0], y)
            break
    if not at:
        res.check("the owner-drawn list's rows are drawn (WM_DRAWITEM)", False, "no dark grey in the dialog")
        return
    x = at[0] + 20
    bands, y = [], at[1]
    while y < c["y"] + c["h"] and len(bands) < 3:
        px, n = im.getpixel((x, y)), 0
        while y < c["y"] + c["h"] and im.getpixel((x, y)) == px:
            n, y = n + 1, y + 1
        bands.append((px, n))
    want = [(dark, 20), ((0, 0, 0), 20), (dark, 20)]
    res.check("an owner-drawn list's rows are the program's: measured 20 tall, the selected one drawn black",
              bands == want, f"bands {bands}")


def caption_line_ends(dbg, qmp, res, title):
    """3.11's dialog caption: the black line under its bar stops inside
    the white edge on each side, so white shows at both ends."""
    win = title and dbg.window(title)
    if not win:
        res.check("the dialog's caption can be measured", False, f"no window {title!r}")
        return
    c = win["content"]
    im = shot(qmp, f"{TMP}/w16_dlg_caption.png")
    # The white edge: the first white pixel in from the left, a few rows down.
    y_mid = c["y"] + 8
    edge = next((x for x in range(c["x"], c["x"] + 12) if im.getpixel((x, y_mid)) == WHITE), None)
    line = None
    right = c["x"] + c["w"] - 1 - (edge - c["x"]) if edge is not None else None
    # The line, found at the RIGHT end: the left one is the control-menu box's.
    if edge is not None:
        line = next((y for y in range(c["y"] + 1, c["y"] + 40) if im.getpixel((right - 1, y)) == BLACK
                     and im.getpixel((right - 1, y - 1)) == NAVY), None)
    ok = line is not None and im.getpixel((edge, line)) == WHITE and im.getpixel((right, line)) == WHITE \
        and im.getpixel((edge + 1, line)) == BLACK and im.getpixel((right - 1, line)) == BLACK
    res.check("the caption's black line stops short of its white edges, white at both ends (3.11's)",
              ok, f"edge x {edge}, line y {line}, ends {line and im.getpixel((edge, line))}/"
                  f"{line and im.getpixel((right, line))}")


def dialogs(dbg, qmp, res):
    sh(dbg, f"rm {DLG_INI}")
    # DlgDirList's folder: two .TXT files and one that does not match, and a folder.
    sh(dbg, "mkdir /home/Win16")
    sh(dbg, "mkdir /home/Win16/W16DIR")
    sh(dbg, "mkdir /home/Win16/W16DIR/SUB")
    for name in ("B.TXT", "A.TXT", "C.DAT"):
        sh(dbg, f"write /home/Win16/W16DIR/{name} x")
    win = dbg.spawn("/bin/w16 /usr/share/w16/dialog.exe", DLG_TITLE)
    res.check("dialog.exe opens its window", win is not None, "no window")
    if not win:
        return
    res.check("a second GetDC while one is held is another DC, the first keeps its pen: NestedDC=1",
              ini_has(dbg, "NestedDC=1"), sh(dbg, f"cat {DLG_INI}"))
    dbg.send("gui key f alt")
    titles = wait_titles(dbg, lambda t: "Popup" in t)
    res.check("Alt+F opens the File menu from the bar", "Popup" in titles, f"windows {titles}")
    dbg.send("gui key c")
    res.check("File > Checked unticks itself (CheckMenuItem): Checked=0", ini_has(dbg, "Checked=0"),
              sh(dbg, f"cat {DLG_INI}"))
    dbg.send("gui key 0xF789")                    # F2
    titles = wait_titles(dbg, lambda t: "Number" in t)
    res.check("F2, an accelerator, opens the dialog", "Number" in titles, f"windows {titles}")
    ini = sh(dbg, f"cat {DLG_INI}")
    got = dict(re.findall(r"(\w+)=(.*)", ini))
    rows = [got.get(f"Dir{i}", "").strip() for i in range(5)]
    res.check("DlgDirList: the .TXT files, then [..] and the folder, sorted; the drives after them",
              got.get("DirOK", "").strip() == "1" and rows == ["a.txt", "b.txt", "[..]", "[sub]", "[-c-]"], rows)
    res.check("...the static shows the folder it made current, in lower case: c:\\w16dir",
              got.get("DirPath", "").strip() == "c:\\w16dir", got.get("DirPath"))
    res.check("DlgDirSelect of [sub] answers sub\\ and TRUE", (got.get("DirSel", "").strip(),
              got.get("DirIsDir", "").strip()) == ("sub\\", "1"), (got.get("DirSel"), got.get("DirIsDir")))
    caption_line_ends(dbg, qmp, res, next((t for t in titles if "Number" in t), None))
    owner_drawn_list(dbg, qmp, res, next((t for t in titles if "Number" in t), None))
    for ch in "57":
        dbg.send(f"gui key {ch}")
        dbg.settle(0.1)
    dbg.send("gui key 0x0a")
    res.check("what is typed over 42 is what GetDlgItemInt reads on OK: Value=57", ini_has(dbg, "Value=57"),
              sh(dbg, f"cat {DLG_INI}"))
    titles = wait_titles(dbg, lambda t: "Number" not in t)
    res.check("EndDialog closes it", "Number" not in titles, f"windows {titles}")
    key_repeat(dbg, qmp, res)
    dbg.send("gui key 0xF789")                    # F2: the Number dialog once more, for its control menu
    titles = wait_titles(dbg, lambda t: "Number" in t)
    dialog_control_menu(dbg, res, next((t for t in titles if "Number" in t), "Number"))
    win = dbg.window(DLG_TITLE)
    dbg.rclick(win["x"] + win["w"] // 2, win["y"] + win["h"] - 20)
    titles = wait_titles(dbg, lambda t: t.count(DLG_TITLE) == 2)
    res.check("a right click's MessageBox opens, captioned as the program asked", titles.count(DLG_TITLE) == 2,
              f"windows {titles}")
    dbg.send("gui key n")
    res.check("its No answers by mnemonic: Box=7 (IDNO)", ini_has(dbg, "Box=7"), sh(dbg, f"cat {DLG_INI}"))
    # Control Panel's Mouse swaps the buttons in WIN.INI: a LEFT click is then the program's right one.
    sh(dbg, "write /home/Win16/WINDOWS/WIN.INI [windows]")
    sh(dbg, "append /home/Win16/WINDOWS/WIN.INI SwapMouseButtons=1")
    try:
        dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] - 20)
        titles = wait_titles(dbg, lambda t: t.count(DLG_TITLE) == 2)
        res.check("with WIN.INI's SwapMouseButtons=1 a left click is the program's right one: the box opens",
                  titles.count(DLG_TITLE) == 2, f"windows {titles}")
        if titles.count(DLG_TITLE) == 2:
            dbg.send("gui key n")
            wait_titles(dbg, lambda t: t.count(DLG_TITLE) == 1)
    finally:
        sh(dbg, "rm /home/Win16/WINDOWS/WIN.INI")
    always_on_top(dbg, res)
    dbg.send("gui key h alt")
    wait_titles(dbg, lambda t: "Popup" in t)
    dbg.send("gui key a")
    titles = wait_titles(dbg, lambda t: "About Dialog test" in t)
    res.check("Help > About is ShellAbout's box", "About Dialog test" in titles, f"windows {titles}")
    dbg.send("gui key 0x0a")
    titles = wait_titles(dbg, lambda t: "About Dialog test" not in t)
    res.check("...and Enter closes it", "About Dialog test" not in titles, f"windows {titles}")
    dbg.close_window(DLG_TITLE)
    # THE POINTER LANDS ON THE ROW UNDER IT, down to the last one: File >
    # Exit clicked by place, from the popup's own rectangle (the last row
    # ends 2 px above the 2 px shadow). Rows hit-tested in the DESKTOP's
    # font height drifted further each row (2026-10-19, on the ASUS); the
    # desktop font is made larger than w16's for this, or the two match
    # and a drift cannot show, then put back.
    was = (sh(dbg, "config get system.font.size").strip().split() or ["16"])[-1]
    sh(dbg, "config set system.font.size 24")
    try:
        drift_check(dbg, res)
    finally:
        sh(dbg, f"config set system.font.size {was}")


def always_on_top(dbg, res):
    """The program's own control-menu item (GetSystemMenu + AppendMenu), chosen by its mnemonic, is
    WM_SYSCOMMAND to it; its SetWindowPos(HWND_TOPMOST) is toywm's Always on Top, and NOTOPMOST
    takes it off again -- Clock's menu item."""
    for want in (1, 0):
        win = dbg.window(DLG_TITLE)
        dbg.click(win["x"] + 10, win["y"] + 10)     # the control-menu box, drawn by w16 itself
        dbg.settle(0.5)
        dbg.send("gui key t")
        ok = ini_has(dbg, f"Top={want}")
        end = time.time() + 3
        while (above := (dbg.window(DLG_TITLE) or {}).get("above")) is not bool(want) and time.time() < end:
            time.sleep(0.3)
        res.check(f"the control menu's own Always on Top item reaches the program: Top={want}", ok,
                  sh(dbg, f"cat {DLG_INI}"))
        res.check(f"...and its SetWindowPos({'HWND_TOPMOST' if want else 'HWND_NOTOPMOST'}) "
                  f"{'pins' if want else 'unpins'} the window in toywm", above is bool(want), f"above {above!r}")


# 3.11's VGA driver's dither of (200,100,50), MEASURED on the reference (Paintbrush's Edit Colors swatch):
# by the program's screen y % 8 (rows) and x % 8. A dark grey, B olive, C red, D yellow, as VGA showed them.
DITHER_200_100_50 = ["CDADCDAD", "CACBCACB", "ADACADAC", "CACACACA", "CDADCCAD", "CACBCACB", "ADACADAC", "CACACACA"]
DITHER_COLOURS = {"A": (135, 136, 143), "B": (168, 168, 87), "C": (255, 0, 0), "D": (255, 255, 0)}


def colours16(dbg, qmp, res, tmp):
    """colors=16 for gdi.exe: its (200,100,50) brush is 3.11's VGA dither, pixel for pixel, laid on the
    program's screen (the client at FRAME, FRAME + CAPTION); its pure colours stay solid."""
    sh(dbg, f"write {GDI_CONF} colors=16")
    win = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", GDI_TITLE)
    res.check("gdi.exe opens with colors=16", win is not None, "no window")
    if win:
        dbg.settle(0.5)
        im = shot(qmp, f"{tmp}/w16_dither.png")
        ox, oy = win["x"] + FRAME, win["y"] + FRAME + CAPTION
        bad = [(x, y, im.getpixel((ox + x, oy + y)))
               for y in range(62, 70) for x in range(100, 116)
               if im.getpixel((ox + x, oy + y)) != DITHER_COLOURS[DITHER_200_100_50[(y + FRAME + CAPTION) % 8][(x + FRAME) % 8]]]
        res.check("the (200,100,50) brush is 3.11's VGA dither, every pixel where the reference has it",
                  not bad, f"{len(bad)} of 128 differ, first {bad[:4]}")
        px = lambda x, y: im.getpixel((ox + x, oy + y))
        res.check("...and a colour the VGA has stays solid: the red line, the blue rectangle",
                  px(16, 50) == RED and px(79, 50) == RED and px(30, 64) == BLUE,
                  f"line {px(16, 50)}/{px(79, 50)} fill {px(30, 64)}")
        dbg.close_window(GDI_TITLE)
    sh(dbg, f"rm {GDI_CONF}")


def popup_window(dbg, qmp, res, tmp):
    """A second top-level window: popup.exe's WS_POPUP the size of the screen it is told of (640 x 480)
    covers SaunaOS's whole screen, the 640 x 480 centred on black, its origin where its green corner says;
    a click in it is the program's, which destroys it, and the main window stays -- View Picture's shape."""
    win = dbg.spawn("/bin/w16 /usr/share/w16/popup.exe", POPUP_TITLE)
    res.check("popup.exe opens its main window", win is not None, "no window")
    if not win:
        return
    im = shot(qmp, f"{tmp}/w16_popup.png")
    sw, sh = im.size
    full = None
    end = time.time() + 6
    while time.time() < end and not full:
        full = next((w for w in dbg.windows() if w["w"] == sw and w["h"] == sh and w.get("client_pid")), None)
        if not full:
            time.sleep(0.3)
    res.check("a popup the size of the reported screen covers SaunaOS's whole screen", full is not None,
              f"{[(w['title'], w['w'], w['h']) for w in dbg.windows()]}")
    if not full:
        return
    dbg.settle(0.5)
    im = shot(qmp, f"{tmp}/w16_popup.png")
    ox, oy = (sw - 640) // 2, (sh - 480) // 2
    px = im.getpixel
    res.check("...its 640 x 480 centred on black: the green corner at the centred origin, red beyond it",
              px((ox + 5, oy + 5)) == GREEN and px((ox + 30, oy + 5)) == RED and px((sw // 2, sh // 2)) == RED
              and px((ox + 639, oy + 479)) == RED,
              f"origin {px((ox + 5, oy + 5))} beside {px((ox + 30, oy + 5))} middle {px((sw // 2, sh // 2))} "
              f"far corner {px((ox + 639, oy + 479))}")
    res.check("...and black round it, nothing of the desktop showing",
              px((ox - 3, oy + 5)) == BLACK and px((ox + 5, oy - 3)) == BLACK and px((3, 3)) == BLACK
              and px((sw - 3, sh - 3)) == BLACK,
              f"left {px((ox - 3, oy + 5))} above {px((ox + 5, oy - 3))} corners {px((3, 3))}/{px((sw - 3, sh - 3))}")
    dbg.click(sw // 2, sh // 2)
    gone = False
    end = time.time() + 6
    while time.time() < end and not gone:
        gone = not any(w["w"] == sw and w["h"] == sh and w.get("client_pid") for w in dbg.windows())
        if not gone:
            time.sleep(0.3)
    res.check("a click is the program's: it destroys the popup, and the main window stays",
              gone and dbg.window(POPUP_TITLE) is not None, f"{[(w['title'], w['w'], w['h']) for w in dbg.windows()]}")
    dbg.close_window(POPUP_TITLE)


PLACE_TITLE = "Place test"


def placed_popup(dbg, qmp, res, tmp):
    """place.exe's popup, placed by its program (WIN_REQ_DIALOG_PLACE): opened at its 40,30 from the
    main window's corner and drawn there; a click is the program's GetWindowRect then MoveWindow 10 to
    the right -- DK's encyclopaedia's way of keeping a panel put, which a GetWindowRect answering 0,0
    sent to the corner; the main window dragged carries it along; and dragged by its own caption, the
    program hears where to, so its next GetWindowRect starts from there."""
    win = dbg.spawn("/bin/w16 /usr/share/w16/place.exe", PLACE_TITLE)
    res.check("place.exe opens its main window", win is not None, "no window")
    if not win:
        return

    def pair():
        ws = dbg.windows()
        main = next((w for w in ws if w["title"] == PLACE_TITLE and not w.get("dialog")), None)
        pop = next((w for w in ws if main and w.get("dialog") and w.get("client_pid") == main.get("client_pid")), None)
        return main, pop

    def wait_place(want, t=6.0):
        got = []

        def ready():
            got[:] = pair()
            return got[1] and got[1].get("place") and want(got[1]["place"])
        poll(ready, t)
        return got[0], got[1]

    main, pop = wait_place(lambda p: True)
    res.check("its popup opens as a placed window", pop is not None and pop.get("place") is not None,
              f"{[(w['title'], w.get('dialog'), w.get('place')) for w in dbg.windows()]}")
    if not pop:
        dbg.close_window(PLACE_TITLE)
        return
    s = pop["content"]["w"] / 120          # w16's scale: the popup is 120 wide in the program's pixels
    px = lambda v: round(v * s)
    p = pop["place"]
    rel = (pop["content"]["x"] - main["content"]["x"], pop["content"]["y"] - main["content"]["y"])
    res.check("...at the program's 40,30 from the main window's corner, where toywm put it",
              (p["x"], p["y"]) == (px(40), px(30)) and rel == (p["x"], p["y"]), f"place {p} at {rel}, scale {s}")
    dbg.settle(0.5)
    im = shot(qmp, f"{tmp}/w16_place.png")
    cx, cy = pop["content"]["x"] + pop["content"]["w"] // 2, pop["content"]["y"] + pop["content"]["h"] * 2 // 3
    res.check("...and drawn there: its red client area", im.getpixel((cx, cy)) == RED, f"{im.getpixel((cx, cy))} at {cx},{cy}")

    dbg.click(cx, cy)
    main, pop = wait_place(lambda q: q["x"] == px(50))
    res.check("a click is GetWindowRect then MoveWindow 10 to the right: at 50,30",
              pop and (pop["place"]["x"], pop["place"]["y"]) == (px(50), px(30)), f"place {pop and pop['place']}")

    # Its own caption first: a press in the main window's content -- w16 draws that caption -- raises
    # the main window over its dialogs (toywm's content press, bring_to_front()).
    bx, by = pop["x"] + pop["w"] // 2, pop["y"] + px(6)
    dbg.drag_real(qmp, bx, by, bx + 30, by + 20, steps=6)
    main, pop = wait_place(lambda q: q["x"] != px(50))
    p = pop and pop["place"]
    res.check("dragged by its own caption, it stays attached at the new offset",
              p and abs(p["x"] - (px(50) + 30)) <= 1 and abs(p["y"] - (px(30) + 20)) <= 1, f"place {p}")
    if p:
        want = p["x"] + px(10)
        dbg.click(pop["content"]["x"] + pop["content"]["w"] // 2, pop["content"]["y"] + pop["content"]["h"] * 2 // 3)
        main, pop = wait_place(lambda q: abs(q["x"] - want) <= 1)
        res.check("...and the program heard where to: its next GetWindowRect moves it on from there",
                  pop and abs(pop["place"]["x"] - want) <= 1 and abs(pop["place"]["y"] - p["y"]) <= 1,
                  f"place {pop and pop['place']}, wanted x {want}")

    if main and pop:
        before = (pop["content"]["x"] - main["content"]["x"], pop["content"]["y"] - main["content"]["y"])
        mx, my = main["content"]["x"] + main["content"]["w"] // 2, main["content"]["y"] + px(10)
        dbg.drag_real(qmp, mx, my, mx + 60, my + 40, steps=6)
        main2, pop2 = wait_place(lambda q: True)
        moved = main2 and (main2["content"]["x"] - main["content"]["x"], main2["content"]["y"] - main["content"]["y"])
        rel = pop2 and (pop2["content"]["x"] - main2["content"]["x"], pop2["content"]["y"] - main2["content"]["y"])
        res.check("the main window dragged carries the popup along, at the same offset",
                  moved == (60, 40) and rel == before, f"main moved {moved}, popup at {rel}, was {before}")
    for _ in range(3):                           # the popup and the main window share the title
        if dbg.window(PLACE_TITLE):
            dbg.close_window(PLACE_TITLE)


def drift_check(dbg, res):
    win = dbg.spawn("/bin/w16 /usr/share/w16/dialog.exe", DLG_TITLE)
    if not win:
        res.check("dialog.exe opens again for the menu check", False, "no window")
        return
    dbg.send("gui key f alt")
    wait_titles(dbg, lambda t: "Popup" in t)
    pop = next((w for w in dbg.windows() if w["title"] == "Popup"), None)
    if pop:
        x, y = pop["x"] + 30, pop["y"] + pop["h"] - 4 - 6
        dbg.send(f"gui move {x} {y}")
        dbg.settle(0.3)
        dbg.click(x, y)
    titles = wait_titles(dbg, lambda t: DLG_TITLE not in t)
    res.check("a click on File's last row, Exit, chooses Exit", DLG_TITLE not in titles,
              f"popup {pop and (pop['x'], pop['y'], pop['w'], pop['h'])}, windows {titles}")
    dbg.close_window(DLG_TITLE)


CALLS = "retrolauncher-calls: layout "


def calls_log(dbg):
    """The calls window's report since the last read: its `rows` line and
    the parts it describes, by name."""
    out = {}
    for line in dbg.logs("retrolauncher-calls:", clear=True):
        m = re.search(r"rows (\d+) show (-?\d) first (\S+) (\S+) copied (-?\d+)", line)
        if m:
            out["rows"], out["show"] = int(m.group(1)), int(m.group(2))
            out["first"], out["copied"] = f"{m.group(3)} {m.group(4)}", int(m.group(5))
        m = re.search(CALLS + r"(\w+)\.(\w+) (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            out[f"{m.group(1)}.{m.group(2)}{m.group(3)}"] = tuple(int(v) for v in m.groups()[3:])
        m = re.search(CALLS + r"(\w+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(CALLS + r"(\w+\.\w+) (-?\d+)$", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def wait_calls(dbg, pred, t=8.0):
    end, got = time.time() + t, {}
    while time.time() < end:
        got.update(calls_log(dbg))
        if pred(got):
            return got
        time.sleep(0.3)
    return got


WHY_TITLE = "IsIconic - partly there"
MISSING_TITLE = "ExitWindows - not there yet"


def why_link(dbg, res):
    """The details panel's "Partly there" list: IsIconic's Why? link opens
    the same window the calls window's Why? does."""
    dbg.logs("retrolauncher", clear=True)
    dbg.send(f"gui spawn {LAUNCHER_BIN} --show /usr/share/w16/fault.exe")
    if not wait_title(dbg, "Retro Launcher"):
        res.check("Retro Launcher opens for the Why? link", False, "")
        return
    layout_lines(dbg, "retrolauncher", lambda a: layout_rect(a, "why 0") is not None)   # drawn once
    dbg.send("gui resize 1200 700")
    dbg.settle()
    time.sleep(1.5)
    lines = dbg.logs("retrolauncher:", clear=True)   # the LAST report is after the resize
    r, win = layout_rect(lines, "why 0"), dbg.window("Retro Launcher")
    if not r or not win:
        res.check("the panel reports IsIconic's Why? link", False, f"{r} {win}")
    else:
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(win["content"]["x"] + r[0] + r[2] // 2, win["content"]["y"] + r[1] + r[3] // 2)
        titles = wait_titles(dbg, lambda t: WHY_TITLE in t)
        why = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": why " in ln]
        res.check("the panel's Why? beside IsIconic opens its explanation",
                  WHY_TITLE in titles and any(ln.endswith("why USER.31 shown") for ln in why),
                  f"windows {titles} log {why}")
        dbg.close_window(WHY_TITLE)
    # ...and the "Missing:" line's What? beside ExitWindows, its explanation too.
    dbg.logs("retrolauncher:", clear=True)
    dbg.send("gui resize 1200 701")              # a fresh frame, so its layout is reported again
    lines = layout_lines(dbg, "retrolauncher", lambda a: layout_rect(a, "what 0") is not None)
    r, win = layout_rect(lines, "what 0"), dbg.window("Retro Launcher")
    if not r or not win:
        res.check("the panel reports ExitWindows's What? link", False, f"{r} {win}")
    else:
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(win["content"]["x"] + r[0] + r[2] // 2, win["content"]["y"] + r[1] + r[3] // 2)
        titles = wait_titles(dbg, lambda t: MISSING_TITLE in t)
        why = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": why " in ln]
        res.check("the panel's What? beside ExitWindows opens its explanation",
                  MISSING_TITLE in titles and any(ln.endswith("why USER.7 shown") for ln in why),
                  f"windows {titles} log {why}")
        dbg.close_window(MISSING_TITLE)
    dbg.close_window("Retro Launcher")


def calls(dbg, res):
    dbg.logs("retrolauncher-calls:", clear=True)
    win = dbg.spawn("/bin/wm/apps/retrolauncher --calls /usr/share/w16/fault.exe", "Fault test - calls")
    res.check("`retrolauncher --calls` opens the program's calls window", win is not None,
              f"windows {[w['title'] for w in dbg.windows()]}")
    if not win:
        return
    lay = wait_calls(dbg, lambda l: "rows" in l and "modules.slot1" in l and "show.slot3" in l)
    res.check("fault.exe opens on Missing, its one row USER.7 ExitWindows",
              lay.get("rows") == 1 and lay.get("show") == 3 and lay.get("first") == "USER.7 ExitWindows",
              f"{ {k: lay.get(k) for k in ('rows', 'show', 'first')} }")
    res.check("two module chips, All and USER", "modules.slot1" in lay and "modules.slot2" not in lay,
              f"slots {[k for k in lay if k.startswith('modules.slot')]}")
    check = sh(dbg, "w16 --check /usr/share/w16/fault.exe")
    imports = int(re.search(r"imports (\d+)", check).group(1))
    res.check("w16 --check names IsIconic a stand-in, and why",
              "partly 1" in check and "call partly USER.31 IsIconic" in check and "\talways no" in check,
              f"{[l for l in check.splitlines() if 'partly' in l]}")
    by = sh(dbg, "w16 --check /usr/share/w16/zbyname.exe")
    named = [ln for ln in by.splitlines() if ln.startswith("call ")]
    res.check("w16 --check LISTS the imports it counts: by name too, each once (it counted them and named none)",
              "imports 3" in by and "has 1" in by and len(named) == 3
              and any(ln.startswith("call missing MYDLL.0 Frobnicate ") for ln in named)
              and any(ln.startswith("call missing KERNEL.0 NoSuchFunction ") for ln in named), f"{by[:300]}")
    cx, cy = win["content"]["x"], win["content"]["y"]

    def chip(i):
        c = lay.get(f"show.slot{i}")
        if c:
            dbg.click(cx + c[0] + c[2] // 2, cy + c[1] + c[3] // 2)
        return c

    chip(2)
    lay.update(wait_calls(dbg, lambda l: l.get("show") == 2))
    res.check("the Partly chip lists the stand-in, USER.31 IsIconic",
              lay.get("show") == 2 and lay.get("rows") == 1 and lay.get("first") == "USER.31 IsIconic",
              f"{ {k: lay.get(k) for k in ('rows', 'show', 'first')} }")
    # Why? on its row: the call's section of the calls page, in a
    # window of its own -- and a Missing row's empty cell opens nothing.
    lay.update(wait_calls(dbg, lambda l: "calls.col6" in l and "calls.row_h" in l and "calls.header_h" in l))
    col, rh, hh = lay.get("calls.col6"), lay.get("calls.row_h"), lay.get("calls.header_h")
    if col and rh and hh:
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(cx + col[0] + col[2] // 3, cy + col[1] + hh + rh // 2)
        titles = wait_titles(dbg, lambda t: WHY_TITLE in t)
        why = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": why " in ln]
        res.check("Why? on IsIconic's row opens its explanation in a window of its own",
                  WHY_TITLE in titles and any(ln.endswith("why USER.31 shown") for ln in why),
                  f"windows {titles} log {why}")
        dbg.close_window(WHY_TITLE)
    else:
        res.check("the calls table reports its Why? column", False, f"{col} {rh} {hh}")
    chip(0)
    lay.update(wait_calls(dbg, lambda l: l.get("show") == 0))
    res.check("the All chip lists every import w16 --check counts",
              lay.get("show") == 0 and lay.get("rows") == imports, f"rows {lay.get('rows')} of {imports}")
    # About on the first row, a call that works: its section of the calls
    # page, generated from the comment above its handler.
    first = lay.get("first", "")
    if col and rh and hh and first:
        name = first.split()[-1]
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(cx + col[0] + col[2] // 3, cy + col[1] + hh + rh // 2)
        titles = wait_titles(dbg, lambda t: f"{name} - about" in t)
        why = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": why " in ln]
        res.check(f"About on {first}'s row (it works) opens its description",
                  f"{name} - about" in titles and any(ln.endswith(f"why {first.split()[0]} shown") for ln in why),
                  f"windows {titles} log {why}")
        dbg.close_window(f"{name} - about")
        # A double click on the row itself, anywhere on it, does what the About cell does.
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(cx + col[0] // 4 + 10, cy + col[1] + hh + rh // 2, settle=False)
        dbg.click(cx + col[0] // 4 + 10, cy + col[1] + hh + rh // 2)
        titles = wait_titles(dbg, lambda t: f"{name} - about" in t)
        res.check(f"a double click on {first}'s row opens its About",
                  f"{name} - about" in titles, f"windows {titles}")
        dbg.close_window(f"{name} - about")
    s = lay.get("search")
    if s:
        dbg.click(cx + s[0] + 10, cy + s[1] + s[3] // 2)
    for ch in "Exit":
        dbg.send(f"gui key {ch}")
        dbg.settle(0.1)
    lay = wait_calls(dbg, lambda l: l.get("rows") == 1)
    res.check("a search narrows the list to what matches", lay.get("rows") == 1, f"rows {lay.get('rows')}")
    t = lay.get("calls")
    if t:
        dbg.click(cx + t[0] + 40, cy + t[1] + 40)
    dbg.send("gui key 0x03")
    lay = wait_calls(dbg, lambda l: l.get("copied", -1) >= 0)
    res.check("Ctrl+C copies the rows shown", lay.get("copied") == 1, f"copied {lay.get('copied')}")
    # ExitWindows's row (Missing): its What? opens its section of the missing
    # calls' page. The table has not moved since its Why? column was read above.
    if col and rh and hh:
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(cx + col[0] + col[2] // 3, cy + col[1] + hh + rh // 2)
        titles = wait_titles(dbg, lambda t: MISSING_TITLE in t)
        why = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": why " in ln]
        res.check("a Missing row's What? opens what ExitWindows is, and why it is not there",
                  MISSING_TITLE in titles and any(ln.endswith("why USER.7 shown") for ln in why),
                  f"windows {titles} log {why}")
        dbg.close_window(MISSING_TITLE)
    out = sh(dbg, "w16 --why USER.31")
    res.check("`w16 --why USER.31` prints IsIconic's section", "IsIconic" in out and "Always says no" in out,
              out[-300:])
    out = sh(dbg, "w16 --why USER.7")
    res.check("...and a missing call's: what ExitWindows does and why it is not there",
              "exitwindows" in out.lower() and "Why it is not there" in out and "CPWIN386.CPL" in out, out[-300:])
    out = " ".join(sh(dbg, "w16 --why USER.43").split())     # rendered at 80 columns: lines wrap
    res.check("...a missing call with no text written says so (CloseWindow, a Windows call)",
              "closewindow" in out.lower() and "No description is written" in out and "Windows calls" in out, out[-300:])
    out = " ".join(sh(dbg, "w16 --why FAKEDLL.9").split())
    res.check("...and one with no name names what w16 knows: a DLL's own export",
              "knows no name" in out and "FAKEDLL.DLL" in out, out[-300:])
    dbg.close_window("Fault test - calls")

    win = dbg.spawn("/bin/wm/apps/retrolauncher --calls /usr/share/w16/hello.exe", "Hello - calls")
    lay = wait_calls(dbg, lambda l: "rows" in l)
    res.check("a program that lacks nothing, with no stand-ins, opens on All",
              win is not None and lay.get("show") == 0 and lay.get("rows", 0) > 0, f"{lay}")
    dbg.close_window("Hello - calls")
    # gdi.exe lacks nothing but plays a metafile, whose arcs are a stand-in.
    win = dbg.spawn("/bin/wm/apps/retrolauncher --calls /usr/share/w16/gdi.exe", "GDI test - calls")
    lay = wait_calls(dbg, lambda l: "rows" in l)
    res.check("...one with a stand-in opens on Partly, the stand-in its row",
              win is not None and lay.get("show") == 2 and lay.get("first") == "GDI.123 PlayMetaFile", f"{lay}")
    dbg.close_window("GDI test - calls")


TMP = "/tmp"


SHAPES_TITLE = "Shapes (Win16)"
# Measured off real 3.11 drawing shapes.exe: each top row's first pixel from the box's left.
ELLIPSE_60x40 = [23, 18, 15, 13, 11, 9, 8, 6, 5, 4, 3, 3, 2, 1, 1, 1, 0, 0, 0, 0]
CORNER_20x16 = [7, 5, 3, 2, 1, 1, 0, 0]


def shapes(dbg, qmp, res, tmp):
    win = dbg.spawn("/bin/w16 /usr/share/w16/shapes.exe", SHAPES_TITLE)
    res.check("shapes.exe opens its window", win is not None, "no window")
    if not win:
        return
    ox, oy = win["x"] + FRAME, win["y"] + FRAME + CAPTION
    dbg.settle()
    im = shot(qmp, f"{tmp}/w16_shapes.png")
    def first_ink(l, r, y):
        xs = [x for x in range(l, r) if im.getpixel((ox + x, oy + y)) != WHITE]
        return xs[0] - l if xs else None
    got = [first_ink(150, 210, 10 + y) for y in range(20)]
    res.check("Ellipse's rows start where 3.11's did", got == ELLIPSE_60x40, f"got {got}")
    got = [first_ink(10, 70, 10 + y) for y in range(8)]
    res.check("RoundRect's corners are 3.11's", got == CORNER_20x16, f"got {got}")
    radius = [next((x for x in range(246, 254) if im.getpixel((ox + x, oy + y)) == BLACK), None) for y in (66, 72, 73, 80)]
    res.check("a pie's radius steps across a pixel where 3.11's did", radius == [250, 250, 249, 249], f"got {radius}")
    filled = [im.getpixel((ox + x, oy + y)) not in (WHITE, BLACK) for x, y in ((249, 70), (248, 80))]
    res.check("the pie's brush reaches its radius", all(filled), f"got {filled}")
    dbg.close_window(SHAPES_TITLE)


CURSOR_TITLE = "Cursors (Win16)"
CLIENT_SHAPE, NO_SHAPE = 14, 13          # wm_internal.h's WM_CURSOR_CLIENT, WM_CURSOR_NONE


def own_cursor(dbg, qmp, res, tmp):
    """cursor.exe's class cursor is its own (cursor.asm's header): an inverting crosshair,
    hotspot (15, 15), a black block 12 up-left of it and a white one 12 down-right, over a
    window black on its left 140 pixels and white on the rest."""
    win = dbg.spawn("/bin/w16 /usr/share/w16/cursor.exe", CURSOR_TITLE)
    res.check("cursor.exe opens its window", win is not None, "no window")
    if not win:
        return
    ox, oy = win["x"] + FRAME, win["y"] + FRAME + CAPTION

    def at(x, y, name):
        dbg.warp_cursor(qmp, ox + x, oy + y)
        dbg.settle()
        c = dbg.state()["cursor"]
        return c.get("shape"), c.get("image"), shot(qmp, f"{tmp}/w16_cursor_{name}.png"), ox + x, oy + y

    im = shot(qmp, f"{tmp}/w16_cursor_drawn.png")    # DrawIcon of cursor 1 at 250,140 of the client
    got = [im.getpixel((ox + 250 + dx, oy + 140 + dy)) for dx, dy in ((3, 3), (21, 15), (21, 21))]
    res.check("DrawIcon draws a cursor: its black block, an inverting arm black over white, the rest left",
              got == [BLACK, BLACK, WHITE], f"got {got}")
    shape, image, im, hx, hy = at(220, 80, "white")
    res.check("over its window the pointer is the program's own cursor", (shape, image) == (CLIENT_SHAPE, 0),
              f"shape {shape} image {image}")
    got = [im.getpixel((hx + dx, hy + dy)) for dx, dy in ((6, 0), (0, -6), (-12, -12), (6, 6))]
    res.check("over white: the inverting arms black, the black block black, the rest white",
              got == [BLACK, BLACK, BLACK, WHITE], f"got {got}")
    shape, image, im, hx, hy = at(60, 80, "black")
    got = [im.getpixel((hx + dx, hy + dy)) for dx, dy in ((6, 0), (0, -6), (12, 12), (6, 6))]
    res.check("over black: the arms inverted to white, the white block white, the rest black",
              got == [WHITE, WHITE, WHITE, BLACK], f"got {got}")
    dbg.click(ox + 220, oy + 80)                 # SetClassWord(GCW_HCURSOR, the square)
    shape, image, im, hx, hy = at(224, 84, "square")
    res.check("SetClassWord(GCW_HCURSOR) gives the class's windows its other cursor from the next move",
              (shape, image) == (CLIENT_SHAPE, 1) and im.getpixel((hx, hy - 10)) == BLACK,
              f"shape {shape} image {image}, top edge {im.getpixel((hx, hy - 10))}")
    dbg.send(f"gui rclick {ox + 224} {oy + 84}")  # SetCursor(NULL)
    dbg.settle()
    shape = dbg.cursor_shape()
    res.check("SetCursor(NULL) takes the pointer away", shape == NO_SHAPE, f"shape {shape}")
    shape, image, _, _, _ = at(230, 90, "back")
    res.check("...and the next move brings the class cursor back", (shape, image) == (CLIENT_SHAPE, 1),
              f"shape {shape} image {image}")
    at(60, 80, "before-key")
    dbg.key("x")                                  # its black block rewritten white, then SetCursor
    dbg.settle()
    c = dbg.state()["cursor"]
    im = shot(qmp, f"{tmp}/w16_cursor_rewritten.png")
    got = im.getpixel((ox + 60 - 12, oy + 80 - 12))
    res.check("a cursor whose bits the program rewrote shows the new bits at its next SetCursor",
              (c.get("shape"), c.get("image")) == (CLIENT_SHAPE, 0) and got == WHITE,
              f"shape {c.get('shape')} image {c.get('image')}, the block {got}")
    dbg.close_window(CURSOR_TITLE)


FPU_INI = "/home/Win16/WINDOWS/FPUTEST.INI"


def coprocessor(dbg, qmp, res):
    sh(dbg, f"rm {FPU_INI}")
    sh(dbg, "spawn /bin/w16 /usr/share/w16/fpu.exe")
    end, text = time.time() + 15, ""
    while time.time() < end and "Root=" not in text:
        time.sleep(0.5)
        text = sh(dbg, f"cat {FPU_INI}")
    got = dict(re.findall(r"^(\w+)=(\S*)", text, re.M))
    try:
        where = "KVM" if "enabled" in (qmp.hmp("info kvm") or "") else "TCG"
    except RuntimeError:                         # remote_gui.py: no QEMU in front, a real x87
        where = "hardware"
    want = "8381" if where == "TCG" else "83818A"   # TCG's x87 has no stack fault
    res.check(f"fpu.exe: each exception reaches its handler once ({where}: {want})",
              got.get("Codes") == want and got.get("Calls") == f"{len(want) // 2:04X}", f"the file: {text!r}")
    res.check("fpu.exe: masked, 1/0 calls nothing and BX=8 reports it",
              got.get("Status") == "0004", f"Status={got.get('Status')}")
    res.check("fpu.exe: sqrt(2) * 1e9 through FISTP is 1414213562", got.get("Root") == "544B2FBA",
              f"Root={got.get('Root')}")


CALL_INI = "/home/Win16/WINDOWS/WINCALLS.INI"


def calls_answered(dbg, res):
    """wincalls.exe asks for what needs no window and writes each answer to WINCALLS.INI; the
    values are what Windows 3.11 answers (the sample's header lists them)."""
    sh(dbg, f"rm {CALL_INI}")
    sh(dbg, "spawn /bin/w16 /usr/share/w16/wincalls.exe")
    end, text = time.time() + 15, ""
    while time.time() < end and "Pal=" not in text:
        time.sleep(0.5)
        text = sh(dbg, f"cat {CALL_INI}")
    got = dict(re.findall(r"^(\w+)=(.*)$", text, re.M))
    want = {
        "EqualSame": "0001", "EqualDiff": "0000",
        "Upper": "ABC XYZ1", "UpperN": "0007",
        "Wvs": "42|toy|beef|100000|07|   05|to", "WvsN": "001E",
        "WinDir": "C:\\WINDOWS", "SysDir": "C:\\WINDOWS\\SYSTEM",
        "Lru": "0001",
        "BkOld": "00FFFFFF", "Bk": "00123456", "TextOld": "00000000", "Text": "00654321",
        "AlignOld": "0000", "Align": "0006", "Pal": "0001",
        "HugeSize": "00030000", "HugeTile": "005A", "HugeFree": "0000",
        "Free": "01000000", "Temp": "3A43", "Drv0": "0000", "Drv2": "0003",
        "Int2f1686": "0000", "Int2f1600": "0A03", "Rop2": "000D", "Org": "00000000",
        "MoveRet": "0001", "MovePt": "0014000A", "ExtRet": "0001",
        "EnvOff": "0000", "EnvHead": "4150", "KbCp": "01B5", "Msw": "0013",
        "BitsSet": "00000008", "BitsGet": "00000008", "BitsOut": "00F9FF04F9FF0004",
        "DibW": "0004", "DibH": "0002",
        # bottom row first (the bitmap's second), each pixel B G R: red, white, black, blue 0,0,128
        "Dib24": "0000FF" "FFFFFF" "000000" "800000" "000000" "0000FF" "FFFFFF" "800000",
        "ReSame": "0001", "ReSize": "00012000", "ReKept": "0077", "ReTile": "0033", "ReHandle": "0001",
        "ReFree": "0000", "Int2f1680": "1600", "Int2f1684": "0000", "Int2f4300": "4300",
        "FloodRet": "0001", "FloodMono": "F800F800", "OleRelease": "0001",
    }
    for names, what in ((("EqualSame", "EqualDiff"), "EqualRect tells equal rectangles from different ones"),
                        (("Upper", "UpperN"), "AnsiUpperBuff upcases its count and no more"),
                        (("Wvs", "WvsN"), "wvsprintf: %d %s %x %lu, a precision on a number and a string, and the length"),
                        (("WinDir", "SysDir"), "GetWindowsDirectory and GetSystemDirectory"),
                        (("Lru",), "GlobalLRUNewest answers its handle"),
                        (("BkOld", "Bk", "TextOld", "Text"), "SetBkColor and SetTextColor return the old colour and read back"),
                        (("AlignOld", "Align"), "SetTextAlign returns the old word and GetTextAlign reads it back"),
                        (("Pal",), "CreatePalette answers a handle"),
                        (("HugeSize", "HugeTile", "HugeFree"), "a 192 KB GlobalAlloc is a run of selectors: its size, a byte in its third 64 KB, freed"),
                        (("Free", "Temp"), "GetFreeSpace and GetTempDrive"),
                        (("Drv0", "Drv2"), "GetDriveType: A: is none, C: fixed"),
                        (("Int2f1686", "Int2f1600"), "INT 2Fh answers protected mode and the Windows version"),
                        (("Rop2", "Org"), "GetROP2 and GetBrushOrg of a new DC"),
                        (("MoveRet", "MovePt"), "MoveToEx answers TRUE and the old position in a POINT"),
                        (("ExtRet",), "GetTextExtentPoint answers TRUE"),
                        (("EnvOff", "EnvHead"), "GetDOSEnvironment points at the environment's PATH="),
                        (("KbCp",), "GetKBCodePage answers 437, the US layout's"),
                        (("Msw",), "SMSW answers 0013 (PE MP ET), as 3.11 does"),
                        (("BitsSet", "BitsGet", "BitsOut"), "SetBitmapBits and GetBitmapBits round-trip a bitmap's bytes"),
                        (("DibW", "DibH", "Dib24"), "GetDIBits fills the header and gives 24-bit rows, bottom row first, in true colours"),
                        (("ReSame", "ReSize", "ReKept", "ReTile", "ReHandle", "ReFree"),
                         "GlobalReAlloc that must move a block past 64 KB keeps its handle and its bytes"),
                        (("Int2f1680", "Int2f1684", "Int2f4300"), "INT 2Fh answers the time slice and no VxD, and leaves a call nobody owns"),
                        (("FloodRet", "FloodMono"), "ExtFloodFill on a 1-bit bitmap stops at a border as that bitmap holds it"),
                        (("OleRelease",), "OleRevokeServer calls the server's Release (Paintbrush's Exit waits for it)")):
        res.check(f"wincalls.exe: {what}", all(got.get(n) == want[n] for n in names),
                  "; ".join(f"{n}={got.get(n)!r} (want {want[n]!r})" for n in names if got.get(n) != want[n]))
    # The size depends on the font, so the two calls are held to each other, and to something.
    res.check("wincalls.exe: GetTextExtentPoint's SIZE is GetTextExtent's answer",
              got.get("Ext") == got.get("ExtPt") and got.get("Ext") not in (None, "00000000"),
              f"Ext={got.get('Ext')} ExtPt={got.get('ExtPt')}")


OPEN_INI ="/home/Win16/WINDOWS/OPENDLG.INI"


def open_dialog(dbg, res):
    """opendlg.exe runs GetOpenFileName three times on a template of its own; its hook writes what it saw
    and what came back to OPENDLG.INI (the sample's header says what each key means)."""
    sh(dbg, f"rm {OPEN_INI}")
    sh(dbg, "spawn /bin/w16 /usr/share/w16/opendlg.exe")
    end, text = time.time() + 20, ""
    while time.time() < end and "Ro3=" not in text:
        time.sleep(0.5)
        text = sh(dbg, f"cat {OPEN_INI}")
    got = dict(re.findall(r"^(\w+)=(.*)$", text, re.M))
    want = {"Vis1": "0000", "Ret1": "0000", "Vis2": "0003", "Ret2": "0001", "Ro2": "0001", "Ret3": "0001", "Ro3": "0000"}
    for names, what in ((("Vis1", "Ret1"), "a template's Read Only and Help hidden by OFN_HIDEREADONLY and no OFN_SHOWHELP"),
                        (("Vis2",), "...and shown when the flags ask for them"),
                        (("Ret2", "Ro2"), "Read Only ticked before OK comes back as OFN_READONLY"),
                        (("Ret3", "Ro3"), "Read Only unticked before OK clears OFN_READONLY")):
        res.check(f"opendlg.exe: {what}", all(got.get(n) == want[n] for n in names),
                  "; ".join(f"{n}={got.get(n)!r} (want {want[n]!r})" for n in names if got.get(n) != want[n]))


# --- The Retro Launcher round (2026-11-01): missing calls answered 0 by choice, Run with trace, its INI
# file, a drive's Type and Label, CD-ROM drives in the sidebar ------------------------------------------

FAULT_CONF = "/etc/win16/usr_share_w16_fault.exe.conf"
FAULT_INI = "/home/Win16/WINDOWS/FAULT.INI"
CD_ROOT = "/home/W16CD"
OPTIONS_TITLE = "Retro Launcher Options"


def last_of(dbg, name):
    return sh(dbg, f"cat {LAST.format(name)}")


def footer(lines):
    """The newest `layout footer-text L1|L2` the launcher logged, as (l1, l2), or None."""
    got = None
    for ln in lines:
        m = re.search(r": layout footer-text (.*)\|(.*)$", ln)
        if m:
            got = (m.group(1), m.group(2))
    return got


def layout_words(lines, name):
    """The newest `layout NAME ...` line's words after the name, or None."""
    got = None
    for ln in lines:
        m = re.search(r": layout " + re.escape(name) + r" (.*)$", ln)
        if m:
            got = m.group(1)
    return got


def inside(inner, outer, slack=0):
    x, y, w, h = inner
    ox, oy, ow, oh = outer
    return x >= ox - slack and y >= oy - slack and x + w <= ox + ow + slack and y + h <= oy + oh + slack


def overlaps(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def show_launcher(dbg, path, ok):
    """Retro Launcher opened on `path` (--show), and its layout lines until `ok` says they are enough."""
    dbg.logs("retrolauncher", clear=True)
    dbg.send(f"gui spawn {LAUNCHER_BIN} --show {path}")
    if not wait_title(dbg, "Retro Launcher"):
        return None
    return layout_lines(dbg, "retrolauncher", ok, timeout=10.0)


def fault_runs_on(dbg, res, what):
    """fault.exe is up: its right click calls ExitWindows, which w16 lacks. Answered 0, it runs on: no
    Application Error box, the window still there. Closed after; True when it ran on."""
    win = poll(lambda: dbg.window(FAULT_TITLE), 10)
    if not win:
        res.check(f"{what}: fault.exe opens", False, f"windows {[w['title'] for w in dbg.windows()]}")
        return False
    dbg.rclick(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    dbg.settle()
    box = poll(lambda: any(w["popup"] for w in dbg.windows()), 3)
    alive = dbg.window(FAULT_TITLE) is not None and not box
    if box:
        dbg.key("0x0a")
    poll(lambda: dbg.window(FAULT_TITLE) is None, 10)
    if dbg.window(FAULT_TITLE):
        dbg.close_window(FAULT_TITLE)
    return alive


def fault_stops(dbg):
    """fault.exe is up: its right click calls ExitWindows; the Application Error box, Enter, the end."""
    win = poll(lambda: dbg.window(FAULT_TITLE), 10)
    if not win:
        return False
    dbg.rclick(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    if not poll(lambda: any(w["popup"] for w in dbg.windows()), 6):
        return False
    dbg.key("0x0a")
    return bool(poll(lambda: dbg.window(FAULT_TITLE) is None, 10))


def panel_overflow(res, lines, what):
    """No footer control or link sticks out of the details panel, none overlaps another, and the INI
    file's button sits inside the panel too -- the overflow rule, from the launcher's own layout lines."""
    panel = layout_words(lines, "panel")
    if not panel:
        res.check(f"{what}: the panel reports where it is", False, "no `layout panel` line")
        return
    p = tuple(int(v) for v in panel.split())
    rects = {n: layout_rect(lines, n) for n in ("run", "run-more", "calls", "ini")}
    link = layout_rect(lines, "footer-link")
    if link and link[2]:
        rects["footer-link"] = link
    out = [n for n, r in rects.items() if r and not inside(r, p)]
    res.check(f"{what}: Run, its arrow, the calls button, the INI button and the footer's link all lie "
              "inside the details panel", not out and rects["run"] and rects["run-more"],
              f"panel {p}, outside: {[(n, rects[n]) for n in out]}, rects {rects}")
    names = [n for n in ("run", "run-more", "calls", "footer-link") if rects.get(n)]
    clash = [(a, b) for i, a in enumerate(names) for b in names[i + 1:] if overlaps(rects[a], rects[b])]
    res.check(f"{what}: the footer's controls do not overlap", not clash, f"{clash} {rects}")


def retro_round(dbg, qmp, res):
    for f in (LAST.format("fault"), "/var/lib/win16/usr_share_w16_fault.exe.trace", FAULT_CONF, FAULT_INI,
              WIN16_CONF):
        sh(dbg, f"rm {f}")

    # 1. The program's own "Missing calls" setting: answered 0, fault.exe runs on, and its record says so.
    sh(dbg, f"write {FAULT_CONF} missing=zero")
    dbg.send("gui spawn /bin/w16 /usr/share/w16/fault.exe")
    ran_on = fault_runs_on(dbg, res, "missing=zero")
    last = last_of(dbg, "fault")
    res.check("missing=zero: ExitWindows is answered 0, fault.exe runs on, and its record names the call",
              ran_on and " ended" in last and "zero USER.7 ExitWindows" in last, f"ran on {ran_on}, the file {last!r}")

    # 2. The details panel: the Missing calls row, the footer's count and its "which?".
    lines = show_launcher(dbg, "/usr/share/w16/fault.exe", lambda a: footer(a) and layout_rect(a, "missing"))
    foot = footer(lines or [])
    res.check("the details panel has the program's Missing calls row",
              lines is not None and layout_rect(lines, "missing") is not None, "no `missing` dropdown")
    res.check("the footer says how many missing calls the last run answered 0, and offers which",
              foot is not None and "1 missing call" in foot[0] and foot[1] == "answered 0 -- which?", f"footer {foot}")
    if lines:
        panel_overflow(res, lines, "after a run that answered 0")
    # A short window, so the panel scrolls: which? brings "Answered 0 last run" to the top.
    dbg.logs("retrolauncher:", clear=True)
    dbg.send("gui resize 1000 380")
    lines = layout_lines(dbg, "retrolauncher", lambda a: layout_rect(a, "footer-link") and layout_words(a, "scroll"))
    link, win = layout_rect(lines, "footer-link"), dbg.window("Retro Launcher")
    if link and link[2] and win:
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(win["content"]["x"] + link[0] + link[2] // 2, win["content"]["y"] + link[1] + link[3] // 2)
        lines = layout_lines(dbg, "retrolauncher", lambda a: (layout_words(a, "scroll") or "0 -1").split()[0] != "0")
        sc = [int(v) for v in (layout_words(lines, "scroll") or "0 -1").split()]
        res.check("which? scrolls the panel to the calls answered 0", sc[1] > 0 and sc[0] == sc[1], f"scroll {sc}")
    else:
        res.check("the footer reports its which? link", False, f"{link} {win}")
    dbg.close_window("Retro Launcher")

    # 3. The run card from the File Manager: Run, skipping them -- this once, the setting back to the default.
    sh(dbg, f"rm {FAULT_CONF}")
    sh(dbg, f"rm {LAST.format('fault')}")
    dbg.send(f"gui spawn {LAUNCHER_BIN} /usr/share/w16/fault.exe")
    card = "Run fault.exe?"
    if not wait_title(dbg, card):
        res.check("fault.exe gets the run card", False, f"windows {[w['title'] for w in dbg.windows()]}")
    else:
        w = poll(lambda: (lambda d: d if "run-alt" in d and "run" in d else None)(dbg.widgets(card)), 6) or {}
        cw = dbg.window(card)["content"]
        alt, run = w.get("run-alt"), w.get("run")
        rect = lambda r: (r["x"], r["y"], r["w"], r["h"])
        body = (0, 0, cw["w"], cw["h"])
        res.check("the run card offers Run, skipping them beside Run, inside the card",
                  alt is not None and run is not None and alt["x"] + alt["w"] <= run["x"]
                  and inside(rect(alt), body) and inside(rect(run), body), f"widgets {w}, card {cw}")
        if alt:
            dbg.click(alt["screen"]["x"] + alt["w"] // 2, alt["screen"]["y"] + alt["h"] // 2)
            ran_on = fault_runs_on(dbg, res, "Run, skipping them")
            last = last_of(dbg, "fault")
            res.check("Run, skipping them runs fault.exe with ExitWindows answered 0, its own setting untouched",
                      ran_on and "zero USER.7 ExitWindows" in last and "missing" not in sh(dbg, f"cat {FAULT_CONF}"),
                      f"ran on {ran_on}, the file {last!r}")
        if dbg.window(card):
            dbg.close_window(card)

    # 4. The split Run button: its menu above it, Run with trace, and Open the trace after the stop.
    sh(dbg, f"rm {LAST.format('fault')}")
    lines = show_launcher(dbg, "/usr/share/w16/fault.exe", lambda a: layout_rect(a, "run-more") and footer(a))
    win = dbg.window("Retro Launcher")
    more, run = layout_rect(lines or [], "run-more"), layout_rect(lines or [], "run")
    res.check("Run is a split button: Run and its arrow side by side, a pixel apart",
              more is not None and run is not None and more[0] == run[0] + run[2] + 1 and more[1] == run[1],
              f"run {run} arrow {more}")
    if more and win:
        c = win["content"]
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(c["x"] + more[0] + more[2] // 2, c["y"] + more[1] + more[3] // 2)
        lines = layout_lines(dbg, "retrolauncher", lambda a: layout_rect(a, "runmenu.item 0 2") is not None)
        items = [layout_rect(lines, f"runmenu.item 0 {i}") for i in range(3)]
        res.check("the arrow opens Run, Run with trace and Run, skipping missing calls, above the button",
                  all(items) and items[2][1] + items[2][3] <= run[1], f"items {items} run {run}")
        if items[1]:
            dbg.click(c["x"] + items[1][0] + items[1][2] // 2, c["y"] + items[1][1] + items[1][3] // 2)
            stopped = fault_stops(dbg)
            last = last_of(dbg, "fault")
            trace = sh(dbg, "cat /var/lib/win16/usr_share_w16_fault.exe.trace")
            res.check("Run with trace writes the trace and the record names it and the call that stopped it",
                      stopped and "missing USER.7 ExitWindows" in last and
                      "trace /var/lib/win16/usr_share_w16_fault.exe.trace" in last and "USER.7" in trace,
                      f"stopped {stopped}, the record {last!r}, trace {trace[:120]!r}")
            dbg.logs("retrolauncher:", clear=True)
            dbg.send("gui resize 1200 640")
            lines = layout_lines(dbg, "retrolauncher",
                                 lambda a: footer(a) and footer(a)[1] == "Open the trace" and layout_rect(a, "footer-link"))
            foot = footer(lines)
            res.check("after the stop the footer says why, and offers Open the trace",
                      foot is not None and "ExitWindows" in foot[0] and foot[0].startswith("Stopped")
                      and foot[1] == "Open the trace", f"footer {foot}")
            panel_overflow(res, lines, "after a stop with a trace")
            link, win = layout_rect(lines, "footer-link"), dbg.window("Retro Launcher")
            if link and link[2] and win:
                # The stop's "closed unexpectedly" card sits over the footer's corner until it times out.
                poll(lambda: not dbg.state().get("notice"), 20)
                before = {w["title"] for w in dbg.windows()}
                dbg.click(win["content"]["x"] + link[0] + link[2] // 2, win["content"]["y"] + link[1] + link[3] // 2)
                new = poll(lambda: {w["title"] for w in dbg.windows()} - before, 10) or set()
                res.check("Open the trace opens it in Log Viewer", any("Log" in t for t in new), f"new windows {new}")
                for t in new:
                    dbg.close_window(t)
    dbg.close_window("Retro Launcher")

    # 5. Its INI file: None yet and Make one..., which makes it and opens Notepad; then its first lines.
    lines = show_launcher(dbg, "/usr/share/w16/fault.exe", lambda a: layout_words(a, "ini") and layout_rect(a, "ini"))
    ini, btn, win = layout_words(lines or [], "ini"), layout_rect(lines or [], "ini"), dbg.window("Retro Launcher")
    res.check("its INI file: C:\\WINDOWS\\FAULT.INI, none yet", ini == "none C:\\WINDOWS\\FAULT.INI|None yet", f"{ini!r}")
    if btn and win:
        before = {w["title"] for w in dbg.windows()}
        dbg.click(win["content"]["x"] + btn[0] + btn[2] // 2, win["content"]["y"] + btn[1] + btn[3] // 2)
        new = poll(lambda: {w["title"] for w in dbg.windows()} - before, 10) or set()
        made = "FAULT.INI" in sh(dbg, "ls /home/Win16/WINDOWS")
        res.check("Make one... makes FAULT.INI in C:\\WINDOWS and opens it in Notepad",
                  made and any("FAULT.INI" in t or "Notepad" in t for t in new), f"made {made}, new windows {new}")
        for t in new:
            dbg.close_window(t)
    dbg.close_window("Retro Launcher")
    sh(dbg, f"write {FAULT_INI} [Fault]")
    sh(dbg, f"append {FAULT_INI} Key=1")
    lines = show_launcher(dbg, "/usr/share/w16/fault.exe", lambda a: (layout_words(a, "ini") or "").startswith("found"))
    ini = layout_words(lines or [], "ini")
    res.check("...and once it is there, its first lines", ini == "found C:\\WINDOWS\\FAULT.INI|2 lines -- [Fault] Key=1",
              f"{ini!r}")
    dbg.close_window("Retro Launcher")

    # 6. A CD-ROM drive: its own row under Drives in the sidebar, the program in its subfolder listed there.
    sh(dbg, f"mkdir {CD_ROOT}")
    sh(dbg, f"mkdir {CD_ROOT}/APP")
    sh(dbg, f"cp /usr/share/w16/hello.exe {CD_ROOT}/APP/HELLO.EXE")
    sh(dbg, f"write {WIN16_CONF} drive_d={CD_ROOT}")
    sh(dbg, f"append {WIN16_CONF} cd_drives=D")
    sh(dbg, f"append {WIN16_CONF} label_d=DKTEST")
    lines = show_launcher(dbg, "/usr/share/w16/fault.exe", lambda a: layout_int(a, "folders.rows") is not None)
    rows, row_h = layout_int(lines or [], "folders.rows"), layout_int(lines or [], "folders.row_h") or 0
    # All programs, Samples, Win16, the Drives heading, D:.
    res.check("a CD-ROM drive gets its own row under a Drives heading", rows == 5, f"rows {rows}")
    side = dbg.widgets("Retro Launcher").get("folders")
    if side and row_h and rows == 5:
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(side["screen"]["x"] + 40, side["screen"]["y"] + row_h * 4 + row_h // 2)
        got = poll(lambda: [ln for ln in dbg.logs("retrolauncher: folder:", clear=False)], 6) or []
        res.check("...listing the program in its subfolder", any("1 of 1 programs, selected HELLO.EXE" in ln for ln in got),
                  f"log {got}")
    dbg.close_window("Retro Launcher")
    # w16 sees the label set for it.
    sh(dbg, "rm /home/w16trace.txt")
    dbg.send("gui spawn /bin/w16 --trace /home/w16trace.txt /usr/share/w16/hello.exe")
    if poll(lambda: dbg.window(HELLO_TITLE), 10):
        dbg.close_window(HELLO_TITLE)
    out = sh(dbg, "cat /home/w16trace.txt")
    res.check("w16 names drive D: a CD-ROM labelled as label_d says", f"drive D: {CD_ROOT} (CD-ROM), labelled DKTEST" in out,
              f"trace head {out[:300]!r}")

    # 7. Options: General's "When a call is missing"; Drives' Type and Label, saved on OK. The Options
    # window is the launcher's second window: what is in it is in its layout lines, not `gui widgets`.
    sh(dbg, f"write {WIN16_CONF} drive_d={CD_ROOT}")
    show_launcher(dbg, "/usr/share/w16/fault.exe", lambda a: footer(a) is not None)
    dbg.send("gui resize 1200 640")
    lines = layout_lines(dbg, "retrolauncher", lambda a: layout_rect(a, "toolbar.button 9") is not None)
    b, win = layout_rect(lines, "toolbar.button 9"), dbg.window("Retro Launcher")
    if not b or not win:
        res.check("the toolbar reports its Options button", False, f"{b} {win}")
        dbg.close_window("Retro Launcher")
        return
    dbg.logs("retrolauncher options", clear=True)
    dbg.click(win["content"]["x"] + b[0] + b[2] // 2, win["content"]["y"] + b[1] + b[3] // 2)
    opt = layout_lines(dbg, "retrolauncher options", lambda a: layout_rect(a, "missing") and layout_rect(a, "pages"))
    res.check("Options > General has When a call is missing, and its note",
              layout_rect(opt, "missing") is not None and layout_rect(opt, "missing-note") is not None,
              f"{len(opt)} layout lines")
    ow = dbg.window(OPTIONS_TITLE)
    nav, rh = layout_rect(opt, "pages"), layout_int(opt, "pages.row_h")
    if not ow or not nav or not rh:
        res.check("the Options window reports its pages", False, f"{ow} {nav} {rh}")
        dbg.close_window("Retro Launcher")
        return
    oc = ow["content"]
    at = lambda r, fx=0.5: (oc["x"] + r[0] + int(r[2] * fx), oc["y"] + r[1] + r[3] // 2)
    dbg.click(oc["x"] + nav[0] + 20, oc["y"] + nav[1] + rh * 4 + rh // 2)        # Drives, the fifth row (after Sound)
    opt = layout_lines(dbg, "retrolauncher options", lambda a: layout_rect(a, "drive-type") and layout_rect(a, "drive-table"))
    ty, lb, table = layout_rect(opt, "drive-type"), layout_rect(opt, "drive-label"), layout_rect(opt, "drive-table")
    res.check("Options > Drives has the selected drive's Type and Label under the list",
              ty is not None and lb is not None and table is not None and ty[1] > table[1] + table[3],
              f"type {ty} label {lb} table {table}")
    trow, thead = layout_int(opt, "drive-table.row_h"), layout_int(opt, "drive-table.header_h")
    if ty and table and trow and thead:
        dbg.click(oc["x"] + table[0] + 40, oc["y"] + table[1] + thead + trow + trow // 2)   # D:, the second row
        opt = layout_lines(dbg, "retrolauncher options", lambda a: layout_rect(a, "drive-type") is not None)
        ty, lb, ok = layout_rect(opt, "drive-type"), layout_rect(opt, "drive-label"), layout_rect(opt, "ok")
        dbg.click(*at(ty, 0.75))                                                 # CD-ROM
        dbg.click(*at(lb, 0.1))
        for ch in "dk1":
            dbg.key(ch, settle=False)
        dbg.settle()
        if ok:
            dbg.click(*at(ok))
        poll(lambda: dbg.window(OPTIONS_TITLE) is None, 6)
        conf = sh(dbg, f"cat {WIN16_CONF}")
        res.check("OK saves D:'s Type and Label: cd_drives=D, label_d=DK1",
                  "cd_drives=D" in conf and "label_d=DK1" in conf, f"the file {conf!r}")
    if dbg.window(OPTIONS_TITLE):
        dbg.close_window(OPTIONS_TITLE)
    dbg.close_window("Retro Launcher")
    for f in (WIN16_CONF, FAULT_INI, FAULT_CONF, "/home/w16trace.txt"):
        sh(dbg, f"rm {f}")

def main():
    global TMP
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    ap.add_argument("--tmp", default="/tmp", help="where screendumps go")
    ap.add_argument("--only", default=None, help="run just these checks, comma-separated (shapes,own_cursor)")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "w16_gui_test")
    TMP = args.tmp
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    checks = [("gdi_checks", None), ("fault", lambda: fault(dbg, res)), ("settings", lambda: settings(dbg, qmp, res)),
              ("screen_effect", lambda: screen_effect(dbg, qmp, res)),
              ("launcher", lambda: launcher(dbg, res)), ("calls", lambda: calls(dbg, res)),
              ("capture_cursor", lambda: capture_cursor(dbg, qmp, res)),
              ("drives_and_folders", lambda: drives_and_folders(dbg, res)),
              ("from_file_manager", lambda: from_file_manager(dbg, qmp, res)),
              ("panel_and_options", lambda: panel_and_options(dbg, qmp, res)), ("sound_page", lambda: sound_page(dbg, res)),
              ("why_link", lambda: why_link(dbg, res)), ("dialogs", lambda: dialogs(dbg, qmp, res)),
              ("coprocessor", lambda: coprocessor(dbg, qmp, res)), ("calls_answered", lambda: calls_answered(dbg, res)),
              ("open_dialog", lambda: open_dialog(dbg, res)), ("probe", lambda: probe(dbg, res)),
              ("shapes", lambda: shapes(dbg, qmp, res, TMP)), ("own_cursor", lambda: own_cursor(dbg, qmp, res, TMP)),
              ("popup_window", lambda: popup_window(dbg, qmp, res, TMP)),
              ("placed_popup", lambda: placed_popup(dbg, qmp, res, TMP)),
              ("colours16", lambda: colours16(dbg, qmp, res, TMP)),
              ("retro_round", lambda: retro_round(dbg, qmp, res))]
    only = set(args.only.split(",")) if args.only else None
    if only and only - {n for n, _ in checks}:
        sys.exit(f"w16_gui_test: no such check: {', '.join(sorted(only - {n for n, _ in checks}))}")
    try:
        for name, fn in checks:
            if only and name not in only:
                continue
            if fn is None:
                win = gdi_checks(dbg, qmp, res, TMP)
                if win:
                    window_controls(dbg, qmp, res, win)
            else:
                fn()
    finally:
        dbg.close()
    print(f"\nw16_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
