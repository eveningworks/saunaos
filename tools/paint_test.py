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
CTRL = {"a": "0x01", "s": "0x13", "y": "0x19", "z": "0x1a"}
BLACK, WHITE, RED = (0, 0, 0), (255, 255, 255), (0xED, 0x1C, 0x24)


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
