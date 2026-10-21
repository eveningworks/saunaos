#!/usr/bin/env python3
"""w16 running Win16 programs in toywm (docs/w16-design.md), checked by pixel.

The programs are the project's own NASM samples (userland/w16/samples/,
built by tools/gen_ne.py), so the test ships with the tree; no Microsoft
file is involved.

1. **gdi.exe draws everything where it put it** (its header lists the
   places): a string-table string, a bitmap resource put with
   SetDIBitsToDevice and through a memory DC with BitBlt -- SRCCOPY the
   same colours, NOTSRCCOPY every one inverted, and the DIB's rows the
   right way up -- a pen line that stops short of its end point, a brush
   rectangle inside a pen edge, text copied through the global and local
   heaps, a square that appears only once its timer has fired three
   times; and a metafile played at twice its size through MM_ANISOTROPIC
   (PlayMetaFile, lib/uwmf: a NULL-pen rectangle and an ellipse), a PatBlt
   cut by IntersectClipRect inside SaveDC/RestoreDC, and a Polygon.
2. **The window controls**: Alt+Space opens w16's own control menu (a
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
   with its mnemonic (Box=7), and Help > About is ShellAbout's box.
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
   drive C:; the first row's Play plays toy-os's chime, and nothing once
   the Sounds choice is Off (before OK).
14. **Why?** on a stand-in: IsIconic's cell in fault.exe's calls window
   and its link in the details panel each open "IsIconic - partly there"
   with its section of the calls page, and a working call's About opens
   "<name> - about"; a Missing row's empty cell opens nothing; `w16 --why
   USER.31` prints its section and `--why USER.7` is refused.

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

17. **Calls that need no window** (wincalls.exe): EqualRect, AnsiUpperBuff, wvsprintf,
   the Windows and System directories, GlobalLRUNewest, the colour and text-align
   state of a device context, CreatePalette -- each answer written to WINCALLS.INI
   and compared with what Windows 3.11 answers.

Every colour is compared with a tolerance of 0: the client buffer is
32-bit and nothing between it and the screendump blends.
"""
import argparse
import re
import sys
import time

from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
import filemanager_test as fm  # noqa: E402
from harness import Results  # noqa: E402

GDI_TITLE = "GDI test (Win16)"
HELLO_TITLE = "Hello (Win16)"
INI = "/home/Win16/WINDOWS/GDITEST.INI"
GDI_CONF = "/etc/win16/usr_share_w16_gdi.exe.conf"
HELLO_CONF = "/etc/win16/usr_share_w16_hello.exe.conf"
LAST = "/var/lib/win16/usr_share_w16_{}.exe.last"   # what the launcher's "Last run" reads
FAULT_TITLE = "Fault (Win16)"
FRAME, CAPTION = 4, 19     # userland/w16/w16_internal.h's W16_FRAME, W16_CAPTION

RED, GREEN, BLUE, WHITE = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)
YELLOW, CYAN, MAGENTA, BLACK = (255, 255, 0), (0, 255, 255), (255, 0, 255), (0, 0, 0)
# w16 shows 3.11's colours as VGA did (ctl16.c's w16_vga): navy and the face grey shift.
NAVY, FACE = (0, 0, 168), (192, 199, 200)


def shot(qmp, path):
    from PIL import Image
    qmp.screenshot(path)
    with Image.open(path) as im:
        return im.convert("RGB")


def inv(c):
    return tuple(255 - v for v in c)


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
    ini = sh(dbg, f"cat {INI}")
    res.check("WritePrivateProfileString kept the file's line and added its own",
              "Square=1" in ini and "Written=yes" in ini, f"the file: {ini!r}")
    return win


def window_controls(dbg, qmp, res, win):
    w0, h0 = win["w"], win["h"]
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
    mark = fm.ctx_mark(dbg)
    fm.sure_rclick(dbg, qmp, lay.ox + px + pw // 2, lay.oy + lay.rowy[0] + lay.rowh * 2 + lay.rowh // 2)
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
    time.sleep(1)
    w = dbg.widgets("Retro Launcher")
    res.check("a short window: fault.exe's Run is in the footer, its Drive C: row scrolled out of view",
              "run" in w and "drive-c" not in w, f"widgets {sorted(w)}")
    det = w.get("details")
    if det:
        dbg.warp_cursor(qmp, *dbg.widget_center("details", "Retro Launcher"))
        dbg.wheel(-10)
        time.sleep(0.5)
        w = dbg.widgets("Retro Launcher")
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
    WIN.INI on drive C:), each Play cell playing toy-os's sound for it --
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
    dbg.click(oc["x"] + pg[0] + 30, oc["y"] + pg[1] + 2 * rh[0] + rh[0] // 2)   # the third page
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
        res.check("the first row's Play plays toy-os's chime for the Default Beep",
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
    win = dbg.spawn("/bin/w16 /usr/share/w16/dialog.exe", DLG_TITLE)
    res.check("dialog.exe opens its window", win is not None, "no window")
    if not win:
        return
    dbg.send("gui key f alt")
    titles = wait_titles(dbg, lambda t: "Popup" in t)
    res.check("Alt+F opens the File menu from the bar", "Popup" in titles, f"windows {titles}")
    dbg.send("gui key c")
    res.check("File > Checked unticks itself (CheckMenuItem): Checked=0", ini_has(dbg, "Checked=0"),
              sh(dbg, f"cat {DLG_INI}"))
    dbg.send("gui key 0xF789")                    # F2
    titles = wait_titles(dbg, lambda t: "Number" in t)
    res.check("F2, an accelerator, opens the dialog", "Number" in titles, f"windows {titles}")
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
    win = dbg.window(DLG_TITLE)
    dbg.rclick(win["x"] + win["w"] // 2, win["y"] + win["h"] - 20)
    titles = wait_titles(dbg, lambda t: t.count(DLG_TITLE) == 2)
    res.check("a right click's MessageBox opens, captioned as the program asked", titles.count(DLG_TITLE) == 2,
              f"windows {titles}")
    dbg.send("gui key n")
    res.check("its No answers by mnemonic: Box=7 (IDNO)", ini_has(dbg, "Box=7"), sh(dbg, f"cat {DLG_INI}"))
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
    # ExitWindows's row (Missing) has no Why?: its cell opens nothing. The
    # table has not moved since its Why? column was read above.
    if col and rh and hh:
        dbg.logs("retrolauncher:", clear=True)
        dbg.click(cx + col[0] + col[2] // 3, cy + col[1] + hh + rh // 2)
        dbg.settle(0.6)
        why = [ln for ln in dbg.logs("retrolauncher:", clear=True) if ": why " in ln]
        res.check("a Missing row's empty Why? cell opens nothing", not why and
                  WHY_TITLE not in [w["title"] for w in dbg.windows()], f"{why}")
    out = sh(dbg, "w16 --why USER.31")
    res.check("`w16 --why USER.31` prints IsIconic's section", "IsIconic" in out and "Always says no" in out,
              out[-300:])
    out = sh(dbg, "w16 --why USER.7")
    res.check("...and refuses a call w16 does not answer (ExitWindows)", "no call w16 answers" in out, out[-200:])
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
        "Wvs": "42|toy|beef|100000", "WvsN": "0012",
        "WinDir": "C:\\WINDOWS", "SysDir": "C:\\WINDOWS\\SYSTEM",
        "Lru": "0001",
        "BkOld": "00FFFFFF", "Bk": "00123456", "TextOld": "00000000", "Text": "00654321",
        "AlignOld": "0000", "Align": "0006", "Pal": "0001",
    }
    for names, what in ((("EqualSame", "EqualDiff"), "EqualRect tells equal rectangles from different ones"),
                        (("Upper", "UpperN"), "AnsiUpperBuff upcases its count and no more"),
                        (("Wvs", "WvsN"), "wvsprintf: %d %s %x %lu, and the length"),
                        (("WinDir", "SysDir"), "GetWindowsDirectory and GetSystemDirectory"),
                        (("Lru",), "GlobalLRUNewest answers its handle"),
                        (("BkOld", "Bk", "TextOld", "Text"), "SetBkColor and SetTextColor return the old colour and read back"),
                        (("AlignOld", "Align"), "SetTextAlign returns the old word and GetTextAlign reads it back"),
                        (("Pal",), "CreatePalette answers a handle")):
        res.check(f"wincalls.exe: {what}", all(got.get(n) == want[n] for n in names),
                  "; ".join(f"{n}={got.get(n)!r} (want {want[n]!r})" for n in names if got.get(n) != want[n]))


def main():
    global TMP
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    ap.add_argument("--tmp", default="/tmp", help="where screendumps go")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "w16_gui_test")
    TMP = args.tmp
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        win = gdi_checks(dbg, qmp, res, TMP)
        if win:
            window_controls(dbg, qmp, res, win)
        fault(dbg, res)
        settings(dbg, qmp, res)
        launcher(dbg, res)
        calls(dbg, res)
        capture_cursor(dbg, qmp, res)
        drives_and_folders(dbg, res)
        from_file_manager(dbg, qmp, res)
        panel_and_options(dbg, qmp, res)
        sound_page(dbg, res)
        why_link(dbg, res)
        dialogs(dbg, qmp, res)
        coprocessor(dbg, qmp, res)
        calls_answered(dbg, res)
        probe(dbg, res)
        shapes(dbg, qmp, res, TMP)
    finally:
        dbg.close()
    print(f"\nw16_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
