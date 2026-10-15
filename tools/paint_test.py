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
  * LAYERS: a stroke on a new layer crossing one on the background; the
    eraser on the upper layer leaves the background's stroke BLACK at the
    crossing (an eraser painting Colour 2 would whiten it); the eye hides
    it; 40% opacity greys it and Ctrl+Z restores it; dragged under the
    opaque background it is covered; Merge down keeps both strokes, and
    Ctrl+Z splits them; saved as .ora and reopened, two layers and the
    same pixels.
  * TEXT LAYERS: a let-go box is a text layer; the Text tool re-opens it
    with its words, an edit stays one layer and one Ctrl+Z; a brush on it
    asks, Make it pixels is a step; saved as .ora and reopened, the words
    open again.
  * SHAPES: S and the bar; smoothing off and Fill: Colour 2; a rectangle
    exact on its edge, inside and out; a corner handle resizing it; a
    click away keeping it as one step (and drawing nothing itself); an
    ellipse smoothed has grey edge pixels the hard one lacks.
  * BRUSHES: the highlighter across a black stroke keeps the black
    BLACK and turns the white EXACTLY its yellow (an "over" blend would
    yellow the black), and a second stroke deepens it by multiplying; a
    soft stroke at hardness 0 has many in-between colours and white past
    its rim, where the Round stroke has none; the spray held still keeps
    adding dots inside its disc (a spray only on motion adds a handful),
    and one Ctrl+Z takes a hold away.
  * COLOURS: the hue strip a third along and the square's top-right
    corner make Colour 1 a full green (G 255, little R or B); R typed as 200 changes only the red; a
    stroke paints exactly that colour.
  * PASTE: Copy picture writes a picture file for the clipboard; Ctrl+V
    of it onto a 256 x 256 picture asks, Expand canvas makes 800 x 600;
    a 48-step drag of it lands within a second (a paste that rewrote the
    layer per motion took 2.1 s), the stroke where it was dragged, and
    ONE Ctrl+Z takes both; Keep floats
    it and its frame drags it 60 left; a corner handle scales it to 700
    x 525 and the stroke lands where that puts it; Ctrl+Shift+V then Esc
    leaves one layer.
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
the reopened checks -- 4 of 14. (2026-10-15) The highlighter blending
UPIX_BLEND_OVER fails both highlighter checks (black turns yellow); a
tools_tick() that never sprays fails the spray check (10 dots, then 20);
the colour square reading saturation backwards fails the green check
(Colour 1 white); Expand without upixundo_join() fails the one-Ctrl+Z
check (the canvas stays 800 x 600); the paste before b1c561d3, which
rewrote the layer on every motion, takes 2.1 s over the drag that must
land within a second.

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
CTRL = {"a": "0x01", "c": "0x03", "s": "0x13", "v": "0x16", "x": "0x18", "y": "0x19", "z": "0x1a"}
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
        # A changed picture's title gains a "*", and a refresh that did not
        # find the window would silently keep the old offsets.
        w = self.dbg.window(self.title) or self.dbg.window("*" + self.title)
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
    check_layers(dbg, qmp, res, tmp)
    check_text_layers(dbg, qmp, res, tmp)
    check_shapes(dbg, qmp, res, tmp)
    check_brushes(dbg, qmp, res, tmp)
    check_colours(dbg, qmp, res, tmp)
    check_paste(dbg, qmp, res, tmp)


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


ORA = "/var/tmp/paint_test.ora"


def layer_stack(dbg):
    """(count, current, opacity) from `paint: layout layerstack`."""
    dbg.settle()
    for line in reversed(dbg.logs("paint: layout layerstack ", clear=False)):
        f = line.split("layerstack ", 1)[1].split()
        return int(f[0]), int(f[2]), int(f[4])
    return 0, -1, -1


def current_is_text(dbg):
    dbg.settle()
    for line in reversed(dbg.logs("paint: layout layerstack ", clear=False)):
        return line.split(" text ", 1)[1].split()[0] == "1"
    return False


def check_text_layers(dbg, qmp, res, tmp):
    """A let-go box is a TEXT LAYER: the Text tool re-opens it with its
    words, an edit is one step, a brush on it asks first, and .ora keeps
    the words."""
    dbg.send(f"sh rm {ORA}")
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Text layers: Paint opens", False, "no window")
        return
    dbg.key("t")
    dbg.click(*scr.at(100, 100))
    type_text(dbg, "Hi")
    dbg.key("0x1b")
    res.check("a let-go text box is a text layer of its own", layer_stack(dbg)[:2] == (2, 1)
              and current_is_text(dbg), f"{layer_stack(dbg)} text {current_is_text(dbg)}")
    dbg.click(*scr.at(104, 110))
    t = text_state(dbg)
    res.check("the Text tool clicked on it opens it again with its words",
              t.get("open") == 1 and t.get("text") == "Hi" and t.get("box", [0])[0] == 100, f"{t}")
    type_text(dbg, "!")
    dbg.key("0x1b")
    res.check("...an edit keeps the one layer", layer_stack(dbg)[0] == 2, f"{layer_stack(dbg)}")
    dbg.key(CTRL["z"], mods="ctrl")
    dbg.click(*scr.at(104, 110))
    t = text_state(dbg)
    res.check("Ctrl+Z takes the edit back, as one step", t.get("text") == "Hi", f"{t}")
    dbg.key("0x1b")
    dbg.key("b")
    dbg.click(*scr.at(110, 112))
    ask = rect(dbg, "ask")
    res.check("a brush on a text layer asks first", ask and ask[2] > 0 and current_is_text(dbg), f"ask {ask}")
    dbg.key("0x0d")   # the primary: Make it pixels
    res.check("...and Make it pixels makes it pixels", not current_is_text(dbg) and layer_stack(dbg)[0] == 2,
              f"{layer_stack(dbg)} text {current_is_text(dbg)}")
    dbg.key(CTRL["z"], mods="ctrl")
    res.check("...as a step Ctrl+Z takes back", current_is_text(dbg), f"text {current_is_text(dbg)}")
    # .ora keeps the words: saved, reopened, the Text tool opens them.
    dbg.key("0x13", mods="ctrl shift")
    time.sleep(0.5)
    dbg.settle()
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in ORA:
        dbg.key({"/": "0x2f", ".": "0x2e", "_": "0x5f"}.get(ch, ch), settle=False)
    dbg.settle()
    dbg.key("0x0d")
    deadline = time.time() + 10
    while time.time() < deadline and state(dbg).get("path") != ORA:
        time.sleep(0.3)
        dbg.settle()
    close_all(dbg)
    dbg.spawn(f"{PAINT} {ORA}", "paint_test.ora - Paint")
    time.sleep(1)
    scr2 = Screen(dbg, qmp, tmp, "paint_test.ora - Paint")
    dbg.key("0", mods="ctrl")
    scr2.refresh()
    dbg.key("t")
    dbg.click(*scr2.at(104, 110))
    t = text_state(dbg)
    res.check("reopened from .ora, the text layer opens with its words",
              scr2.ok and t.get("open") == 1 and t.get("text") == "Hi", f"{t}")
    dbg.key("0x1b")
    close_all(dbg)


def check_layers(dbg, qmp, res, tmp):
    """Layers: a stroke on each of two, the eraser making holes in the top
    one only, hide, opacity, reorder, merge, undo, and an .ora round trip
    -- judged on screen pixels where two strokes CROSS, so a wrong order,
    a white-painting eraser or a lost layer each change what shows."""
    dbg.send(f"sh rm {ORA}")
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Layers: Paint opens", False, "no window")
        return

    def at(part):
        r = rect(dbg, part)
        return (scr.cx + r[0] + r[2] // 2, scr.cy + r[1] + r[3] // 2) if r else None

    def drag(x0, y0, x1, y1):
        a, b = scr.at(x0, y0), scr.at(x1, y1)
        dbg.drag(a[0], a[1], b[0], b[1], steps=16)
        dbg.settle()

    dbg.key("b")
    drag(60, 200, 400, 200)            # Background: a horizontal stroke
    dbg.click(*at("paneltabs.slot 1"))
    dbg.move(10, 10)
    dbg.click(*at("layerbar.button 0"))
    res.check("the Layers tab's + adds a layer above, current", layer_stack(dbg)[:2] == (2, 1), f"{layer_stack(dbg)}")
    drag(200, 60, 200, 350)            # Layer 2: a vertical one, crossing it at (200, 200)
    scr.shoot()
    res.check("a stroke on the new layer shows over the background",
              scr.pixel(200, 300) == BLACK and scr.pixel(300, 200) == BLACK and scr.pixel(300, 300) == WHITE,
              f"{scr.pixel(200, 300)} {scr.pixel(300, 200)} {scr.pixel(300, 300)}")
    # THE ERASER on the top layer: through the crossing, the background's
    # stroke must still show -- an eraser painting Colour 2 would whiten it.
    dbg.key("e")
    drag(200, 120, 200, 230)
    scr.shoot()
    res.check("the eraser on an upper layer makes a hole to the layer below",
              scr.pixel(200, 140) == WHITE and scr.pixel(200, 200) == BLACK and scr.pixel(200, 300) == BLACK,
              f"erased {scr.pixel(200, 140)}, crossing {scr.pixel(200, 200)}, kept {scr.pixel(200, 300)}")
    dbg.click(*at("layers.eye 1"))
    scr.shoot()
    res.check("the eye hides the layer", scr.pixel(200, 300) == WHITE and scr.pixel(300, 200) == BLACK,
              f"{scr.pixel(200, 300)} {scr.pixel(300, 200)}")
    dbg.click(*at("layers.eye 1"))
    op = rect(dbg, "opacity")
    if op:
        dbg.click(scr.cx + op[0] + op[2] * 4 // 10, scr.cy + op[1] + op[3] // 2)
    scr.shoot()
    grey = scr.pixel(200, 300)
    res.check("40% opacity shows the stroke part-way to white",
              60 < grey[0] < 200 and grey[0] == grey[1] == grey[2], f"{grey}, stack {layer_stack(dbg)}")
    dbg.key(CTRL["z"], mods="ctrl")
    scr.shoot()
    res.check("Ctrl+Z puts the opacity back", scr.pixel(200, 300) == BLACK and layer_stack(dbg)[2] == 255,
              f"{scr.pixel(200, 300)} {layer_stack(dbg)}")
    # REORDER by dragging the top row below the bottom one: the opaque
    # white background now covers the stroke.
    top, bottom = rect(dbg, "layers.row 1"), rect(dbg, "layers.row 0")
    if top and bottom:
        x = scr.cx + top[0] + top[2] // 2
        dbg.drag(x, scr.cy + top[1] + top[3] // 2, x, scr.cy + bottom[1] + bottom[3] - 2, steps=16)
        dbg.settle()
    scr.shoot()
    res.check("a row dragged to the bottom goes under the background",
              layer_stack(dbg)[1] == 0 and scr.pixel(200, 300) == WHITE and scr.pixel(300, 200) == BLACK,
              f"stack {layer_stack(dbg)}, {scr.pixel(200, 300)} {scr.pixel(300, 200)}")
    dbg.key(CTRL["z"], mods="ctrl")
    dbg.key("0x05", mods="ctrl")       # Ctrl+E, merge down
    scr.shoot()
    res.check("Merge down leaves one layer showing both strokes",
              layer_stack(dbg)[0] == 1 and scr.pixel(200, 300) == BLACK and scr.pixel(300, 200) == BLACK,
              f"stack {layer_stack(dbg)}, {scr.pixel(200, 300)} {scr.pixel(300, 200)}")
    dbg.key(CTRL["z"], mods="ctrl")
    res.check("...and Ctrl+Z splits them again", layer_stack(dbg)[0] == 2, f"{layer_stack(dbg)}")

    # .ora keeps the layers: saved, reopened in a fresh Paint, two layers
    # and the same pixels -- and the top one can still be hidden alone.
    dbg.key("0x13", mods="ctrl shift")  # Save as
    time.sleep(0.5)
    dbg.settle()
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in ORA:
        dbg.key({"/": "0x2f", ".": "0x2e", "_": "0x5f"}.get(ch, ch), settle=False)
    dbg.settle()
    dbg.key("0x0d")
    deadline = time.time() + 10
    while time.time() < deadline and state(dbg).get("path") != ORA:
        time.sleep(0.3)
        dbg.settle()
    res.check("Save as .ora writes it, and the picture is saved", state(dbg).get("path") == ORA
              and state(dbg).get("dirty") == 0, f"{state(dbg)}")
    close_all(dbg)
    dbg.spawn(f"{PAINT} {ORA}", "paint_test.ora - Paint")
    time.sleep(1)
    scr2 = Screen(dbg, qmp, tmp, "paint_test.ora - Paint")
    if not scr2.ok:
        res.check("the .ora reopens", False, "no window")
        return
    dbg.key("0", mods="ctrl")
    scr2.refresh()
    scr2.shoot()
    res.check("reopened, the .ora has its two layers and both strokes",
              layer_stack(dbg)[0] == 2 and scr2.pixel(200, 300) == BLACK and scr2.pixel(300, 200) == BLACK
              and scr2.pixel(200, 140) == WHITE,
              f"stack {layer_stack(dbg)}, {scr2.pixel(200, 300)} {scr2.pixel(300, 200)} {scr2.pixel(200, 140)}")
    close_all(dbg)


def shape_style(dbg):
    dbg.settle()
    for line in reversed(dbg.logs("paint: layout shapestyle ", clear=False)):
        f = line.split("shapestyle ", 1)[1].split()
        return dict(zip(f[0::2], map(int, f[1::2])))
    return {}


def shape_box(dbg):
    dbg.settle()
    for line in reversed(dbg.logs("paint: layout shape.open ", clear=False)):
        f = line.split("shape.open ", 1)[1].split()
        return [int(v) for v in f[4:8]] if f[0] == "1" else None
    return None


def check_shapes(dbg, qmp, res, tmp):
    """Shapes: the bar's style, a rectangle dragged out and judged by exact
    pixels with smoothing off (outline Colour 1, fill Colour 2), a corner
    handle making it bigger, a click away keeping it as one step, and an
    ellipse smoothed -- grey pixels on its edge where a hard one has none."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Shapes: Paint opens", False, "no window")
        return

    def at(part):
        r = rect(dbg, part)
        return (scr.cx + r[0] + r[2] // 2, scr.cy + r[1] + r[3] // 2) if r else None

    def drag(x0, y0, x1, y1):
        a, b = scr.at(x0, y0), scr.at(x1, y1)
        dbg.drag(a[0], a[1], b[0], b[1], steps=16)
        dbg.settle()

    # Colour 2 red: a right click on the palette's red.
    cell = rect(dbg, "palette.cell 3")
    if cell:
        dbg.rclick(scr.cx + cell[0] + cell[2] // 2, scr.cy + cell[1] + cell[3] // 2)
        dbg.settle()
    dbg.key("s")
    res.check("S is the Shapes tool, and its bar shows", state(dbg).get("tool") == 6 and rect(dbg, "shapegallery"),
              f"{state(dbg)}")
    dbg.click(*at("shapesmooth.button 0"))
    fill = rect(dbg, "shapefill")
    if fill:
        dbg.click(scr.cx + fill[0] + fill[2] // 2, scr.cy + fill[1] + fill[3] // 2)
        dbg.settle()
        dbg.click(scr.cx + fill[0] + fill[2] // 2, scr.cy + fill[1] + 2 * fill[3] + fill[3] // 2)   # row 1
        dbg.settle()
    st = shape_style(dbg)
    res.check("the bar sets smoothing off and Fill: Colour 2", st.get("smooth") == 0 and st.get("fill") == 1
              and st.get("kind") == 2, f"{st}")
    dbg.move(10, 10)
    drag(100, 100, 300, 200)
    scr.shoot()
    # Off the handles: their white middles sit over the outline while it is open.
    res.check("a rectangle: outline Colour 1 on its edge, Colour 2 inside, white outside -- exact",
              scr.pixel(100, 130) == BLACK and scr.pixel(200, 150) == RED and scr.pixel(90, 130) == WHITE
              and scr.pixel(160, 100) == BLACK, f"edge {scr.pixel(100, 130)} in {scr.pixel(200, 150)} "
              f"out {scr.pixel(90, 130)} top {scr.pixel(160, 100)}, box {shape_box(dbg)}")
    h = at("shape.handle 2")
    if h:
        b = scr.at(400, 250)
        dbg.drag(h[0], h[1], b[0], b[1], steps=16)
        dbg.settle()
    scr.shoot()
    res.check("its corner handle makes it bigger, drawn again", shape_box(dbg) == [100, 100, 400, 250]
              and scr.pixel(350, 230) == RED and scr.pixel(400, 230) == BLACK,
              f"box {shape_box(dbg)}, {scr.pixel(350, 230)} {scr.pixel(400, 230)}")
    dbg.click(*scr.at(600, 500))
    scr.shoot()
    res.check("a click away keeps it (and draws nothing of its own)",
              shape_box(dbg) is None and scr.pixel(350, 230) == RED and scr.pixel(600, 500) == WHITE,
              f"box {shape_box(dbg)} {scr.pixel(350, 230)} {scr.pixel(600, 500)}")
    dbg.key(CTRL["z"], mods="ctrl")
    scr.shoot()
    res.check("...as one step Ctrl+Z takes away", scr.pixel(350, 230) == WHITE and scr.pixel(100, 130) == WHITE,
              f"{scr.pixel(350, 230)} {scr.pixel(100, 130)}")

    def greys(y, x0, x1):
        return sum(1 for x in range(x0, x1) if scr.pixel(x, y) not in (WHITE, BLACK, RED))
    # An ellipse, hard, then the same one smoothed.
    dbg.click(*at("shapegallery.button 4"))
    drag(100, 300, 300, 400)
    dbg.key("0x0d")
    scr.shoot()
    hard = greys(330, 95, 160)
    dbg.key(CTRL["z"], mods="ctrl")
    dbg.click(*at("shapesmooth.button 0"))
    drag(100, 300, 300, 400)
    dbg.key("0x0d")
    scr.shoot()
    soft = greys(330, 95, 160)
    res.check("Smooth gives an ellipse soft edges a hard one does not have", hard == 0 and soft > 2,
              f"hard {hard} in-between pixels, smooth {soft}, style {shape_style(dbg)}")
    close_all(dbg)


def brush_state(dbg):
    """The brush size and kind, and the brush settings line's numbers."""
    out = {}
    for line in reversed(dbg.logs("paint: layout tool ", clear=False)):
        p = line.split("layout ", 1)[1].split()
        out.update(size=int(p[6]), kind=int(p[8]))
        break
    for line in reversed(dbg.logs("paint: layout brushset ", clear=False)):
        p = line.split("layout ", 1)[1].split()
        out.update(hardness=int(p[2]), density=int(p[4]))
        break
    return out


def check_brushes(dbg, qmp, res, tmp):
    """Brush kinds: the highlighter over a black stroke, the soft brush's
    edge, the spray held still -- each judged by pixels."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Brushes: Paint opens", False, "no window")
        return
    # Where everything is, read once: a line that does not change is not
    # logged again, and the log is a window.
    parts = {p: rect(dbg, p) for p in ["size", "palette.cell 0", "palette.cell 5", "palette.cell 6",
                                       "palette.cell 8"] + [f"brushkind.button {i}" for i in range(5)]}
    res.check("Brushes: the panel has the five kinds and a Size row", all(parts.values()), f"{parts}")
    if not all(parts.values()):
        close_all(dbg)
        return

    def click(part, frac=0.5):
        r = parts[part]
        dbg.click(scr.cx + r[0] + max(1, int(r[2] * frac)), scr.cy + r[1] + r[3] // 2)
        dbg.settle()

    def drag(*pts):
        for a, b in zip(pts, pts[1:]):
            p0, p1 = scr.at(*a), scr.at(*b)
            dbg.drag(p0[0], p0[1], p1[0], p1[1], steps=16)
            dbg.settle()

    dbg.move(10, 10)
    drag((100, 100), (500, 100))                 # the default Round brush, black, 3 px
    # HIGHLIGHTER, the palette's yellow, about 24 px, across the black
    # stroke. (One stroke crossing itself is upixel_hostcheck's: a
    # scripted drag is a straight line.) A second stroke over the first
    # deepens it, as a real highlighter does.
    click("palette.cell 5")
    click("brushkind.button 4")
    click("size", 24 / 64)
    n = brush_state(dbg).get("size", 0)
    drag((150, 100), (450, 100))
    scr.shoot()
    yellow = (0xFF, 0xF2, 0x00)
    under, band, out = scr.pixel(300, 100), scr.pixel(300, 100 - n // 2 + 2), scr.pixel(300, 100 + n)
    res.check("the highlighter keeps black black and turns white exactly its colour",
              brush_state(dbg).get("kind") == 4 and n >= 16 and under == BLACK and band == yellow and out == WHITE,
              f"size {n}, under {under}, band {band}, outside {out}")
    drag((200, 100), (300, 100))
    scr.shoot()
    twice, once = scr.pixel(250, 100 - n // 2 + 2), scr.pixel(350, 100 - n // 2 + 2)
    deeper = (0xFF, round(0xF2 * 0xF2 / 255), 0)
    res.check("...and a second stroke over it deepens it, multiplied",
              all(abs(a - b) <= 1 for a, b in zip(twice, deeper)) and once == yellow and scr.pixel(250, 100) == BLACK,
              f"twice {twice} (want {deeper}), once {once}, under {scr.pixel(250, 100)}")

    # SOFT, indigo, hardness 0: a soft edge has many in-between values,
    # where the Round brush's black stroke has none.
    click("palette.cell 8")
    click("brushkind.button 2")
    extra = rect(dbg, "brushextra")
    res.check("Soft shows a Hardness row", bool(extra), "no brushextra line")
    if extra:
        parts["brushextra"] = extra
        click("brushextra", 0.0)
    click("size", 24 / 64)
    st = brush_state(dbg)
    drag((100, 300), (500, 300))
    scr.shoot()
    r = st.get("size", 0) // 2
    col = [scr.pixel(300, y) for y in range(300 - r - 3, 300 + r + 4)]
    mids = {c for c in col if c not in (WHITE, (0x3F, 0x48, 0xCC))}
    hard_mids = {scr.pixel(120, y) for y in range(96, 105)} - {WHITE, BLACK}
    centre = scr.pixel(300, 300)
    res.check("a soft stroke fades out: many in-between colours, white past its rim; the hard one none",
              st.get("hardness") == 0 and len(mids) >= 6 and col[0] == WHITE and col[-1] == WHITE
              and abs(centre[2] - 0xCC) <= 3 and not hard_mids,
              f"state {st}, {len(mids)} in-between, rim {col[0]} {col[-1]}, centre {centre}, hard {hard_mids}")

    # SPRAY, green, held still: a short hold, then a long one at the same
    # place -- the dots keep coming with nothing moving.
    click("palette.cell 6")
    click("brushkind.button 3")
    click("size", 30 / 64)
    n = brush_state(dbg).get("size", 0)
    green = (0x22, 0xB1, 0x4C)
    cx, cy = 650, 450

    def hold(seconds):
        dbg.warp_cursor(qmp, *scr.at(cx, cy))
        qmp.mouse_down()
        time.sleep(seconds)
        qmp.mouse_up()
        dbg.settle()
        scr.shoot()
        pts = [(x, y) for y in range(cy - n, cy + n + 1) for x in range(cx - n, cx + n + 1)
               if scr.pixel(x, y) == green]
        return pts

    first = hold(0.3)
    second = hold(1.8)
    outside = [p for p in second if (p[0] - cx) ** 2 + (p[1] - cy) ** 2 > (n / 2 + 1.5) ** 2]
    res.check("the spray keeps spraying while held still, inside its disc",
              n >= 20 and len(first) > 0 and len(second) > len(first) + 100 and not outside,
              f"size {n}, after 0.3 s {len(first)} dots, after 1.8 s more {len(second)}, outside {outside[:3]}")
    dbg.key(CTRL["z"], mods="ctrl")
    scr.shoot()
    back = sum(1 for y in range(cy - n, cy + n + 1) for x in range(cx - n, cx + n + 1) if scr.pixel(x, y) == green)
    res.check("...and Ctrl+Z takes one hold away", back == len(first), f"{back} dots, {len(first)} before the hold")
    close_all(dbg)


def check_colours(dbg, qmp, res, tmp):
    """The colour picker: the hue strip, the square's corner, a typed R,
    each judged by Colour 1 and then by a stroke's pixels."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    parts = {p: rect(dbg, p) for p in ("colour_square", "colour_hue", "colour_r")}
    if not res.check("Colours: the panel has the square, the hue strip and the R field", scr.ok and all(parts.values()),
                     f"{parts}"):
        close_all(dbg)
        return
    sx, sy, sw, sh = parts["colour_square"]
    hx, hy, hw, hh = parts["colour_hue"]
    # Hue 120 (green) on the strip, then the square's top-right corner:
    # full saturation and brightness.
    # A third of the way along is about 120 degrees; the strip's ends are
    # inset by half its thumb, so "about" is all a click can promise.
    dbg.click(scr.cx + hx + hw // 3, scr.cy + hy + hh // 2)
    dbg.click(scr.cx + sx + sw - 2, scr.cy + sy + 1)
    dbg.settle()
    c1 = int(state(dbg).get("c1", "0"), 16)
    r, g, b = (c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF, c1 & 0xFF
    res.check("the strip's green and the square's corner make Colour 1 a full green", g == 255 and max(r, b) < 50
              and min(r, b) == 0, f"c1 {c1:08X}")
    # A typed R: the field takes the number whole.
    rx, ry, rw, rh = parts["colour_r"]
    dbg.click(scr.cx + rx + rw // 2, scr.cy + ry + rh // 2)
    dbg.key(CTRL["a"], mods="ctrl")
    for ch in "200":
        dbg.key(ch)
    dbg.settle()
    c1 = int(state(dbg).get("c1", "0"), 16)
    res.check("R typed as 200 sets Colour 1's red, the rest kept", c1 & 0xFFFFFF == (200 << 16 | 0xFF00 | (c1 & 0xFF))
              and (c1 >> 16) & 0xFF == 200 and (c1 >> 8) & 0xFF == 255, f"c1 {c1:08X}")
    dbg.key("0x1b")
    a, z = scr.at(100, 100), scr.at(400, 100)
    dbg.drag(a[0], a[1], z[0], z[1], steps=12)
    scr.shoot()
    px = scr.pixel(250, 100)
    res.check("...and the brush paints it", px == ((c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF, c1 & 0xFF),
              f"{px} for {c1:08X}")
    close_all(dbg)


def paste_box(dbg):
    for line in reversed(dbg.logs("paint: layout paste.open ", clear=False)):
        p = line.split("paste.open ", 1)[1].split()
        return [int(v) for v in p[2:6]] if p[0] == "1" else None
    return None


def check_paste(dbg, qmp, res, tmp):
    """Copy picture, then Paste onto a smaller picture: Expand grows the
    canvas and one Ctrl+Z takes both; Keep floats it, its frame moves it
    and a corner handle scales it; Esc drops a paste into a new layer
    and the layer with it -- judged by the size, the layer count and
    pixels."""
    dbg.spawn(PAINT, "Untitled - Paint")
    time.sleep(1)
    scr = Screen(dbg, qmp, tmp, "Untitled - Paint")
    if not scr.ok:
        res.check("Paste: Paint opens", False, "no window")
        return

    def drag(x0, y0, x1, y1):
        a, b = scr.at(x0, y0), scr.at(x1, y1)
        dbg.drag(a[0], a[1], b[0], b[1], steps=16)
        dbg.settle()

    def click(part):
        r = rect(dbg, part)
        if r:
            dbg.click(scr.cx + r[0] + r[2] // 2, scr.cy + r[1] + r[3] // 2)
            dbg.settle()
        return r

    drag(100, 100, 300, 100)                    # black, 3 px, across an 800 x 600 picture
    dbg.key(CTRL["c"], mods="ctrl")
    dbg.settle()
    files = dbg.send("sh ls /tmp/clipboard") or ""
    res.check("Copy picture puts a picture file on the clipboard", ".qoi" in files, files.strip()[-120:])
    # A new 256 x 256 picture (Icon), not saving this one.
    dbg.key("0x0e", mods="ctrl")
    dbg.settle()
    click("ask.button 1")                        # Don't save
    click("sizes.card 6")
    click("newdlg.button 1")
    scr.refresh()
    st = state(dbg)
    if not res.check("...and a new 256 x 256 picture to paste it on", st.get("w") == 256 and st.get("h") == 256,
                     f"{st}"):
        close_all(dbg)
        return
    # Bigger than the canvas: it asks. Expand canvas.
    dbg.key(CTRL["v"], mods="ctrl")
    dbg.settle()
    asked = click("ask.button 1")
    scr.refresh()
    st, box = state(dbg), paste_box(dbg)
    res.check("Ctrl+V of a bigger picture asks, and Expand canvas grows it to fit, the paste floating at 0,0",
              bool(asked) and st.get("w") == 800 and st.get("h") == 600 and box == [0, 0, 800, 600],
              f"asked {asked}, {st}, box {box}")
    # IT KEEPS UP WITH THE POINTER: a 48-step drag of the whole 800 x 600
    # paste lands well inside a second (0.12 s measured; 2.1 s when every
    # motion rewrote the layer). Where it ends is not enough -- a lagging
    # one ends in the same place.
    scr.refresh()   # the picture is 800 x 600 now, and laid out again
    p0, p1 = scr.at(400, 300), scr.at(300, 250)
    t0 = time.time()
    dbg.drag(p0[0], p0[1], p1[0], p1[1], settle=False, steps=48)
    landed = None
    while time.time() - t0 < 30:
        if paste_box(dbg) == [-100, -50, 800, 600]:
            landed = time.time() - t0
            break
        time.sleep(0.05)
    res.check("a 48-step drag of an 800 x 600 paste lands within a second", landed is not None and landed < 1.0,
              f"{landed and round(landed, 2)} s, box {paste_box(dbg)}")
    dbg.key("0x0d")
    scr.shoot()
    res.check("Enter places it where it was dragged: the stroke 100 left and 50 up, white above it",
              scr.pixel(100, 50) == BLACK and scr.pixel(100, 40) == WHITE and paste_box(dbg) is None,
              f"{scr.pixel(100, 50)} {scr.pixel(100, 40)}, box {paste_box(dbg)}")
    dbg.key(CTRL["z"], mods="ctrl")
    scr.refresh()
    st = state(dbg)
    res.check("ONE Ctrl+Z takes the paste and the bigger canvas", st.get("w") == 256 and st.get("h") == 256, f"{st}")
    scr.shoot()
    res.check("...back to white", scr.pixel(100, 50) == WHITE, f"{scr.pixel(100, 50)}")

    # Keep canvas size: floating over the edge; its frame drags it left.
    dbg.key(CTRL["v"], mods="ctrl")
    dbg.settle()
    click("ask.button 0")
    drag(128, 128, 68, 128)
    box = paste_box(dbg)
    dbg.key("0x0d")
    scr.shoot()
    st = state(dbg)
    res.check("Keep canvas size floats it; its frame moves it 60 left, and Enter keeps it there",
              box == [-60, 0, 800, 600] and st.get("w") == 256 and scr.pixel(140, 100) == BLACK
              and scr.pixel(140, 90) == WHITE, f"box {box}, {st}, {scr.pixel(140, 100)} {scr.pixel(140, 90)}")
    dbg.key(CTRL["z"], mods="ctrl")

    # A corner handle scales it: the top-left one from 0,0 to 100,75.
    dbg.key(CTRL["v"], mods="ctrl")
    dbg.settle()
    click("ask.button 0")
    h = rect(dbg, "paste.handle 0")
    if h:
        a, b = (scr.cx + h[0] + h[2] // 2, scr.cy + h[1] + h[3] // 2), scr.at(100, 75)
        dbg.drag(a[0], a[1], b[0], b[1], steps=16)
        dbg.settle()
    box = paste_box(dbg)
    dbg.key("0x0d")
    scr.shoot()
    # The stroke, 100..300 at y 100 of 800 x 600, lands at 187..362 and y 162 of the 700 x 525 box.
    dark = min(scr.pixel(220, y)[0] for y in range(160, 166))
    res.check("a corner handle scales it: the stroke lands where the smaller box puts it",
              box == [100, 75, 700, 525] and dark < 120 and scr.pixel(220, 140) == WHITE
              and scr.pixel(150, 100) == WHITE, f"box {box}, darkest {dark}, {scr.pixel(220, 140)} {scr.pixel(150, 100)}")
    dbg.key(CTRL["z"], mods="ctrl")

    # Into a new layer, then Esc: the paste and the layer both go.
    dbg.key(CTRL["v"], mods="ctrl shift")
    dbg.settle()
    click("ask.button 0")
    during = layer_stack(dbg)
    dbg.key("0x1b")
    dbg.settle()
    res.check("Ctrl+Shift+V pastes into a new layer, and Esc drops it and the layer",
              during[0] == 2 and layer_stack(dbg)[0] == 1 and paste_box(dbg) is None,
              f"during {during}, after {layer_stack(dbg)}")
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
