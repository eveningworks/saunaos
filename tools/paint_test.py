#!/usr/bin/env python3
"""tools/paint_test.py -- Paint, end to end, read back from the PICTURE.

Drives userland/gui/apps/paint.c through its own layout lines (`paint:
layout ...`) and judges every step by PIXELS -- the screen's, and, after
a save and a reopen, the file's -- never by what the app says it did.

What it proves, and what a broken version would still pass:

  * Paint opens on an 800 x 600 WHITE picture with its rail, canvas,
    palette and status laid out.
  * A BRUSH DRAG paints: screen pixels along the drag turn black, and a
    control pixel just above the stroke stays white -- a canvas that
    redrew the whole picture black would fail the control.
  * The PALETTE's red and the FILL tool flood the background red AROUND
    the stroke: the stroke stays black, which a fill ignoring the
    region's edge would paint over.
  * UNDO puts the white back and REDO the red, on screen.
  * The RIGHT BUTTON paints the SECOND colour (white) with the pencil --
    a path the toolkit does not route, so Paint drives it by hand.
  * SAVE on an untitled picture asks for a name; the file it writes is a
    PNG of the right size (`imginfo`), and the title loses its `*`.
  * REOPENED in a fresh Paint, the stroke, the fill and the white dot
    are back at the same picture pixels: the file holds what was drawn,
    not what the first window remembered.
  * The RAIL: clicking its pencil button makes the pencil the tool.
  * ZOOM: Ctrl++ takes 100% to 150% and the picture's rect with it.
  * RESIZE: Canvas size to 1000 wide on the left anchor keeps the old
    pixels left and fills the new room with Colour 2; Resize at 50% with
    the aspect kept halves both sides. SAVE AS TYPE: picking JPEG turns
    "Untitled.png" into "Untitled.jpg".
  * HANDLES: the canvas's corner handle dragged by (+50, +25) at 50% makes
    an 800 x 600 picture 900 x 650. TEXT: "Hi" typed into a box puts
    non-white pixels where the box was, and one Ctrl+Z takes them all.
  * TEXT EDITING: Home, Shift+Left, Ctrl+X/V and a click change the
    text and caret the box reports; its frame dragged moves it, leaving
    WHITE behind (a move that forgot to restore would leave the old
    letters); a side handle halves its width into two lines; Ctrl+Z
    undoes the typing and then the box.
  * NEW PICTURE: Ctrl+N shows the size cards; HD + Create makes a 1280 x
    720 picture, and a size typed into both fields picks the Custom card
    and makes that picture.

Positive control (2026-10-14): `paint_tools.c`'s fill painting the
primary colour as the SECOND (`g_colour[!slot]`) reddens the fill, the
undo/redo, the right-click (judged against the red fill beside it) and
the reopened checks -- 4 of 14.

Usage (the VM must already be up):

    python3 tools/vm.py --instance auto start
    python3 tools/paint_test.py --instance <n>
"""
import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from harness import Results  # noqa: E402

PAINT = "/bin/wm/apps/paint"
SAVED = "/var/tmp/paint_test.png"
CTRL = {"a": "0x01", "s": "0x13", "v": "0x16", "x": "0x18", "y": "0x19", "z": "0x1a"}
K_HOME, K_END, K_SHIFT_LEFT = "0xf786", "0xf787", "0xf78b"
BLACK, WHITE, RED = (0, 0, 0), (255, 255, 255), (0xED, 0x1C, 0x24)
K_DOWN = "0xf781"   # KEY_ARROW_DOWN


def layout(dbg, part):
    """The last `paint: layout <part> ...` line's numbers."""
    for line in reversed(dbg.logs(f"paint: layout {part} ", clear=False)):
        try:
            return [int(v, 16) if v.startswith("FF") and len(v) == 8 else int(v)
                    for v in line.split(f"layout {part} ", 1)[1].split()[:4]]
        except ValueError:
            continue
    return None


def state(dbg):
    """tool, colours, brush, shape, zoom -- and the picture's size, dirty
    flag and path."""
    out = {}
    for line in reversed(dbg.logs("paint: layout tool ", clear=False)):
        p = line.split("layout ", 1)[1].split()
        out.update(tool=int(p[1]), c1=p[3], c2=p[4], zoom=int(p[10]))
        break
    for line in reversed(dbg.logs("paint: layout image ", clear=False)):
        p = line.split("layout ", 1)[1].split()
        out.update(w=int(p[1]), h=int(p[2]), dirty=int(p[4]), path=p[6])
        break
    return out


class Screen:
    """Picture pixel -> screen pixel at the current zoom, and reading it."""

    def __init__(self, dbg, qmp, tmp, title):
        self.dbg, self.qmp, self.tmp, self.title = dbg, qmp, tmp, title
        self.n = 0
        self.img = None
        self.refresh()

    def refresh(self):
        self.dbg.settle()
        w = self.dbg.window(self.title)
        pic = layout(self.dbg, "canvas.picture")
        self.ok = bool(w and pic)
        if self.ok:
            self.cx, self.cy = w["content"]["x"], w["content"]["y"]
            self.px, self.py, self.pw, self.ph = pic
            self.zoom = state(self.dbg).get("zoom", 100)

    def at(self, x, y):
        return (self.cx + self.px + x * self.zoom // 100 + self.zoom // 200,
                self.cy + self.py + y * self.zoom // 100 + self.zoom // 200)

    def shoot(self):
        from PIL import Image
        self.n += 1
        path = os.path.join(self.tmp, f"paint{self.n}.png")
        self.dbg.warp_cursor(self.qmp, 1270, 700)   # the sprite must not sit on a sampled pixel
        self.dbg.settle()
        self.qmp.screenshot(path)
        self.img = Image.open(path).convert("RGB")

    def pixel(self, x, y):
        return self.img.getpixel(self.at(x, y))


def close_all(dbg):
    for w in dbg.windows():
        if w.get("client_pid"):
            dbg.send(f"sh kill {w['client_pid']}")
    dbg.settle()


def run(dbg, qmp, res, tmp):
    dbg.send("sh config set desktop.layout_log on")
    dbg.send(f"sh rm {SAVED}")
    close_all(dbg)
    dbg.spawn(PAINT, "Untitled - Paint")
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    st = state(dbg)
    res.check("Paint opens on an 800 x 600 picture, laid out",
              scr.ok and st.get("w") == 800 and st.get("h") == 600 and layout(dbg, "palette.cell 3"),
              f"state {st}, picture {layout(dbg, 'canvas.picture')}")
    if not scr.ok:
        return
    scr.shoot()
    res.check("...and it is white", scr.pixel(400, 300) == WHITE, f"{scr.pixel(400, 300)}")

    # A brush stroke, left to right at y 100.
    x0, y0 = scr.at(100, 100)
    x1, _ = scr.at(300, 100)
    dbg.drag(x0, y0, x1, y0, steps=12)
    scr.shoot()
    along = [scr.pixel(x, 100) for x in (110, 200, 290)]
    res.check("a brush drag paints black along it", all(p == BLACK for p in along), f"{along}")
    res.check("...and the pixel above it stays white", scr.pixel(200, 90) == WHITE,
              f"{scr.pixel(200, 90)}")

    # The palette's red, then the fill tool on the background.
    cell = layout(dbg, "palette.cell 3")
    dbg.click(scr.cx + cell[0] + cell[2] // 2, scr.cy + cell[1] + cell[3] // 2)
    dbg.key("f")
    fx, fy = scr.at(500, 300)
    dbg.click(fx, fy)
    scr.shoot()
    st = state(dbg)
    res.check("the palette's red and the fill tool flood the background",
              scr.pixel(500, 300) == RED and scr.pixel(200, 90) == RED,
              f"c1 {st.get('c1')}, background {scr.pixel(500, 300)}, above the stroke {scr.pixel(200, 90)}")
    res.check("...around the stroke, which stays black", scr.pixel(200, 100) == BLACK,
              f"{scr.pixel(200, 100)}")

    dbg.key(CTRL["z"], mods="ctrl")
    scr.shoot()
    undone = scr.pixel(500, 300)
    dbg.key(CTRL["y"], mods="ctrl")
    scr.shoot()
    res.check("Ctrl+Z puts the white back and Ctrl+Y the red",
              undone == WHITE and scr.pixel(500, 300) == RED, f"undo {undone}, redo {scr.pixel(500, 300)}")

    # The right button: the pencil paints colour 2, white, at one pixel.
    dbg.key("p")
    rx, ry = scr.at(600, 400)
    dbg.rclick(rx, ry)
    scr.shoot()
    res.check("a right-click with the pencil paints the second colour",
              scr.pixel(600, 400) == WHITE and scr.pixel(602, 400) == RED,
              f"at {scr.pixel(600, 400)}, beside {scr.pixel(602, 400)}")

    # The rail's pencil button, after another tool.
    dbg.key("b")
    rail = None
    for line in dbg.logs("paint: layout rail.button 0 ", clear=False)[-1:]:
        rail = [int(v) for v in line.split("rail.button 0 ", 1)[1].split()[:4]]
    if rail:
        dbg.click(scr.cx + rail[0] + rail[2] // 2, scr.cy + rail[1] + rail[3] // 2)
        dbg.settle()
    res.check("the rail's pencil button makes the pencil the tool",
              rail and state(dbg).get("tool") == 0, f"button {rail}, state {state(dbg)}")

    # Save: an untitled picture asks for a name.
    res.check("the picture is unsaved", state(dbg).get("dirty") == 1, f"{state(dbg)}")
    dbg.key(CTRL["s"], mods="ctrl")
    time.sleep(0.5)
    dbg.settle()
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in SAVED:
        dbg.key({"/": "0x2f", ".": "0x2e", "_": "0x5f"}.get(ch, ch), settle=False)
    dbg.settle()
    dbg.key("0x0d")
    deadline = time.time() + 10
    while time.time() < deadline and state(dbg).get("path") != SAVED:
        time.sleep(0.3)
        dbg.settle()
    st = state(dbg)
    info = dbg.send(f"sh imginfo {SAVED}") or ""
    res.check("Ctrl+S asks for a name and saves there", st.get("path") == SAVED and st.get("dirty") == 0,
              f"{st}")
    res.check("...a PNG of 800 x 600", "png" in info.lower() and "800" in info and "600" in info,
              info.strip()[-160:])

    # Reopened: what the FILE holds.
    close_all(dbg)
    dbg.spawn(f"{PAINT} {SAVED}", "paint_test.png - Paint")
    dbg.settle()
    dbg.key("0", mods="ctrl")   # actual size: it opens fitted
    scr2 = Screen(dbg, qmp, tmp, "paint_test.png - Paint")
    if scr2.ok:
        scr2.shoot()
    got = [scr2.pixel(200, 100), scr2.pixel(500, 300), scr2.pixel(600, 400)] if scr2.ok else None
    res.check("reopened, the stroke, the fill and the white dot are in the file",
              got == [BLACK, RED, WHITE], f"{got}")

    # Zoom: 100 -> 150, and the picture's rect with it.
    before = layout(dbg, "canvas.picture")
    dbg.key("=", mods="ctrl")
    dbg.settle()
    after = layout(dbg, "canvas.picture")
    res.check("Ctrl++ zooms to 150% and the picture grows with it",
              state(dbg).get("zoom") == 150 and before and after and after[2] == before[2] * 3 // 2,
              f"zoom {state(dbg).get('zoom')}, {before} -> {after}")
    close_all(dbg)
    dbg.send(f"sh rm {SAVED}")
    check_new(dbg, res)
    check_resize(dbg, qmp, res, tmp)
    check_handles_and_text(dbg, qmp, res, tmp)
    check_text_edit(dbg, qmp, res, tmp)


def rect(dbg, part):
    for line in reversed(dbg.logs(f"paint: layout {part} ", clear=False)):
        try:
            return [int(v) for v in line.split(f"layout {part} ", 1)[1].split()[:4]]
        except ValueError:
            return None
    return None


def check_new(dbg, res):
    """New picture: a preset card, then a typed (Custom) size; judged by
    the picture's size, which a dialog that drew and did nothing fails."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    dbg.settle()
    w = dbg.window("Untitled - Paint")
    if not w:
        res.check("New: Paint opens", False, "no window")
        return
    cx, cy = w["content"]["x"], w["content"]["y"]
    dbg.key("0x0e", mods="ctrl")   # Ctrl+N
    dbg.settle()
    card = rect(dbg, "sizes.card 2")   # HD
    create = rect(dbg, "newdlg.button 1")
    res.check("Ctrl+N opens New picture with its size cards", bool(card and create),
              f"card {card}, Create {create}")
    if not (card and create):
        close_all(dbg)
        return
    dbg.click(cx + card[0] + card[2] // 2, cy + card[1] + card[3] // 2)
    dbg.click(cx + create[0] + create[2] // 2, cy + create[1] + create[3] // 2)
    dbg.settle()
    st = state(dbg)
    res.check("the HD card and Create make a 1280 x 720 picture",
              st.get("w") == 1280 and st.get("h") == 720, f"{st}")
    # Custom: typed into both fields, Tab between them, Enter creates.
    dbg.key("0x0e", mods="ctrl")
    dbg.settle()
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in "640":
        dbg.key(ch, settle=False)
    dbg.key("0x09")
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in "333":
        dbg.key(ch, settle=False)
    dbg.settle()
    sel = None
    for line in reversed(dbg.logs("paint: layout sizes.selected ", clear=False)):
        sel = int(line.split()[-1])
        break
    dbg.key("0x0d")
    dbg.settle()
    st = state(dbg)
    res.check("a typed size picks Custom and makes that picture",
              sel == 7 and st.get("w") == 640 and st.get("h") == 333, f"card {sel}, {st}")
    close_all(dbg)


def check_resize(dbg, qmp, res, tmp):
    """Canvas size and Resize, judged by the picture's size AND its
    pixels; then Save's type list rewriting the name's extension."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Resize: Paint opens", False, "no window")
        return
    cell = layout(dbg, "palette.cell 3")
    dbg.click(scr.cx + cell[0] + cell[2] // 2, scr.cy + cell[1] + cell[3] // 2)   # red
    dbg.key("f")
    fx, fy = scr.at(400, 300)
    dbg.click(fx, fy)
    # Canvas size: 1000 wide, anchored on the RIGHT edge's middle -- the old
    # picture moves right by 200 and the new room is the left 200, in view.
    dbg.key("0x12", mods="ctrl shift")
    dbg.settle()
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in "1000":
        dbg.key(ch, settle=False)
    dbg.settle()
    a = rect(dbg, "anchor.cell 5")
    if a:
        dbg.click(scr.cx + a[0] + a[2] // 2, scr.cy + a[1] + a[3] // 2)
    dbg.key("0x0d")
    scr.refresh()
    scr.shoot()
    st = state(dbg)
    res.check("Canvas size widens the picture to 1000 x 600", st.get("w") == 1000 and st.get("h") == 600,
              f"anchor {a}, {st}")
    res.check("...the new room on the left in Colour 2, the old pixels moved right",
              scr.pixel(100, 300) == WHITE and scr.pixel(196, 300) == WHITE and scr.pixel(204, 300) == RED,
              f"new {scr.pixel(100, 300)}, edge {scr.pixel(196, 300)} | {scr.pixel(204, 300)}")
    # Resize by Percent, aspect kept: 50 -> 500 x 300.
    dbg.key("0x12", mods="ctrl")
    dbg.settle()
    seg = rect(dbg, "by")
    if seg:
        dbg.click(scr.cx + seg[0] + seg[2] // 4, scr.cy + seg[1] + seg[3] // 2)   # Percent
    w_ = "focused at open"
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in "50":
        dbg.key(ch, settle=False)
    dbg.key("0x0d")
    dbg.settle()
    st = state(dbg)
    res.check("Resize at 50% with the aspect kept makes 500 x 300", st.get("w") == 500 and st.get("h") == 300,
              f"by {seg}, width field {w_}, {st}")
    # Save as type: JPEG rewrites "Untitled.png" to "Untitled.jpg".
    dbg.key(CTRL["s"], mods="ctrl")
    time.sleep(0.8)
    dbg.settle()
    name0 = None
    for line in reversed(dbg.logs("filedialog: layout name.text ", clear=False)):
        name0 = line.split("name.text ", 1)[1].strip().strip('"')
        break
    dlg = [w for w in dbg.windows() if w.get("dialog")]
    typ = None
    for line in reversed(dbg.logs("filedialog: layout type ", clear=False)):
        typ = [int(v) for v in line.split("layout type ", 1)[1].split()[:4]]
        break
    name1 = None
    if dlg and typ:
        dx, dy = dlg[-1]["content"]["x"], dlg[-1]["content"]["y"]
        dbg.click(dx + typ[0] + typ[2] // 2, dy + typ[1] + typ[3] // 2)
        dbg.key(K_DOWN)
        dbg.key("0x0d")
        dbg.settle()
        for line in reversed(dbg.logs("filedialog: layout name.text ", clear=False)):
            name1 = line.split("name.text ", 1)[1].strip().strip('"')
            break
    res.check("Save as type JPEG rewrites the name's extension",
              name0 == "Untitled.png" and name1 == "Untitled.jpg", f"{name0!r} -> {name1!r}")
    dbg.key("0x1b")
    close_all(dbg)


def check_handles_and_text(dbg, qmp, res, tmp):
    """The canvas's corner handle, dragged, is a new size; the text tool
    puts letters into the picture, in Colour 1, as one undo step."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    dbg.key("-", mods="ctrl")
    dbg.key("-", mods="ctrl")   # 50%: the picture and its handles in view
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Handles: Paint opens", False, "no window")
        return
    hd = None
    for line in reversed(dbg.logs("paint: layout canvas.handle 3 ", clear=False)):
        hd = [int(v) for v in line.split("canvas.handle 3 ", 1)[1].split()[:4]]
        break
    if hd:
        x0, y0 = scr.cx + hd[0] + hd[2] // 2, scr.cy + hd[1] + hd[3] // 2
        dbg.drag(x0, y0, x0 + 50, y0 + 25, steps=10)   # 50% zoom: +100 x +50 picture px
        dbg.settle()
    st = state(dbg)
    res.check("dragging the corner handle makes the canvas 900 x 650",
              hd and st.get("w") == 900 and st.get("h") == 650, f"handle {hd}, {st}")
    # The text tool: a box at (40, 40), "Hi", Esc.
    dbg.key("t")
    scr.refresh()
    tx, ty = scr.at(40, 40)
    dbg.click(tx, ty)
    for ch in "Hi":
        dbg.key(ch)
    dbg.key("0x1b")
    scr.refresh()
    scr.shoot()

    def inked():
        n = 0
        for py in range(40, 76, 2):
            for px in range(40, 90, 2):
                if scr.pixel(px, py) != WHITE:
                    n += 1
        return n
    ink = inked()
    res.check("the text tool draws letters into the picture", ink > 4, f"{ink} non-white samples")
    dbg.key(CTRL["z"], mods="ctrl")
    scr.shoot()
    res.check("...as one undo step", inked() == 0, f"{inked()} left after Ctrl+Z")
    close_all(dbg)


def text_state(dbg):
    """The open text box: [x, y, w, h], then the rest of its line as a dict."""
    dbg.settle()
    for line in reversed(dbg.logs("paint: layout text ", clear=False)):
        p = line.split("layout text ", 1)[1]
        head, _, typed = p.partition(" text ")
        f = head.split()
        d = {"box": [int(v) for v in f[:4]], "open": int(f[5])}
        if d["open"]:
            d.update(wrap=int(f[7]), cursor=int(f[9]), sel=(int(f[11]), int(f[12])), text=typed)
        return d
    return {"open": 0}


def type_text(dbg, s):
    for ch in s:
        dbg.key("0x20" if ch == " " else ch)


def check_text_edit(dbg, qmp, res, tmp):
    """The open box edits like a text field and moves by its frame."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Text edit: Paint opens", False, "no window")
        return
    dbg.key("t")
    dbg.click(*scr.at(100, 100))
    type_text(dbg, "Hello world")
    dbg.key(K_HOME)
    dbg.key("X")
    t = text_state(dbg)
    res.check("Home puts the caret first: X goes in before the text",
              t.get("text") == "XHello world" and t.get("cursor") == 1, f"{t}")
    dbg.key(K_END)
    for _ in range(5):
        dbg.key(K_SHIFT_LEFT)
    t = text_state(dbg)
    res.check("Shift+Left x5 from the end selects 'world'", t.get("sel") == (7, 12), f"{t}")
    dbg.key(CTRL["x"], mods="ctrl")
    t = text_state(dbg)
    res.check("Ctrl+X cuts it", t.get("text") == "XHello ", f"{t}")
    dbg.key(CTRL["v"], mods="ctrl")
    t = text_state(dbg)
    res.check("...and Ctrl+V puts it back", t.get("text") == "XHello world", f"{t}")
    # A click in the box's left edge puts the caret at 0.
    x, y, w, h = t["box"]
    dbg.click(*scr.at(x + 1, y + h // 2))
    t = text_state(dbg)
    res.check("a click inside the text places the caret", t.get("cursor") == 0, f"{t}")
    # Dragged by its top edge, (+60, +40) at 100%; the old place is white again.
    sx, sy = scr.at(x + w // 2, y)
    dbg.drag(sx, sy - 3, sx + 60, sy - 3 + 40, steps=12)
    t = text_state(dbg)
    res.check("dragging the frame moves the box", t.get("box", [0, 0])[:2] == [x + 60, y + 40], f"{t}")
    scr.shoot()
    old = sum(1 for py in range(y, y + 30, 2) for px in range(x, x + 50, 2) if scr.pixel(px, py) != WHITE)
    nx, ny = t["box"][:2]
    new = sum(1 for py in range(ny, ny + h, 2) for px in range(nx, nx + w, 2) if scr.pixel(px, py) != WHITE)
    res.check("...and its letters go with it", old == 0 and new > 4, f"{old} left behind, {new} at the new place")
    # The right handle dragged to half the width wraps "XHello world" in two.
    x, y, w, h = t["box"]
    sx, sy = scr.at(x + w, y + h // 2)
    dbg.drag(sx + 1, sy, sx - w // 2, sy, steps=12)
    t = text_state(dbg)
    res.check("the side handle sets a wrap width: two lines",
              t.get("wrap", 0) > 0 and t.get("box", [0, 0, 0, 0])[3] == 2 * h, f"{t}, one line was {h}")
    dbg.key("0x1b")
    t = text_state(dbg)
    res.check("Esc lets it go", t.get("open") == 0, f"{t}")
    # A fresh box with nothing typed: Ctrl+Z is the picture's and takes it away.
    dbg.click(*scr.at(300, 300))
    type_text(dbg, "a")
    dbg.key(CTRL["z"], mods="ctrl")
    dbg.key(CTRL["z"], mods="ctrl")
    t = text_state(dbg)
    res.check("Ctrl+Z undoes the typing, then the box", t.get("open") == 0, f"{t}")
    close_all(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "paint_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    res = Results()
    with tempfile.TemporaryDirectory() as tmp, DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, res, tmp)
    print(f"\npaint_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
