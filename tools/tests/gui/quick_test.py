#!/usr/bin/env python3
"""tools/tests/gui/quick_test.py -- Quick Settings (userland/apps/system/toywm/wm_quick.h): the
tray's network-volume-brightness button opens the tiles, each tile acts
through its owner, a chevron opens that owner's flyout as a page with
Back, and the slider writes the setting.

WHAT THIS ASSERTS THAT "THE CARD OPENED" WOULD NOT
-------------------------------------------------
* EACH TILE CHANGES THE REAL THING: Do Not Disturb's setting and the
  bell's state, `desktop.transparency`, the on-screen keyboard's overlay,
  the active keyboard layout -- read back from `config` and `gui state`,
  not from the tile's own idea of itself.
* AN ON TILE IS DRAWN ON: the network tile's face is the accent.
* A HOVERED TILE KEEPS ITS OUTLINE: the wash stops inside the hairline
  on the right edge of a whole tile (it once painted over it), sampled
  against the same pixel at rest, with a pixel inside that must change
  to prove the hover was drawn at all.
* BACK SITS IN A HEADER, NEVER ON A ROW: on the keyboard-layout page
  (and the remote-desktop one, which exists only while a session is
  live), Back's bottom is above the page's first row --
  both once had no header, and Back covered "English (US)".
* THE SLIDER WRITES: a click a quarter along sets `system.volume` to a
  quarter of its range.
* A PAGE IS THE OWNER'S CARD: the network chevron puts up the network
  flyout itself (`gui state`'s overlays), Back returns to the tiles, and
  a click outside closes both.

    python3 tools/lib/vm.py --disk <copy> --audio-wav /dev/null start
    python3 tools/tests/gui/quick_test.py
    echo $?

POSITIVE CONTROL: tile_click() not acting on T_GLASS reddens the
transparency check alone; draw_tile()'s hover fill at `w - 1` on a tile
with no chevron reddens the outline check alone; the page's is_open not followed (g_page left
set after the network card closes) reddens "a click outside closes both".
"""

RUNNER = {
    "covers": "Quick Settings: tiles act through their owners, the slider writes, a page with Back",
}


import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402


def near(p, q, tol=24):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "quick_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="quick_")
    res = Results()

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        for k in ("desktop.notifications.dnd", "desktop.transparency"):
            dbg.send(f"sh config unset {k}")

        def q():
            return dbg.json("gui quick --json")

        def tile(name):
            return next(t for t in q()["tiles"] if t["name"] == name)

        def overlays():
            return dbg.state()["overlays"]

        def cfg(name):
            return (dbg.send(f"sh config get {name}") or "").strip()

        def click_tile(name, chevron=False):
            t = tile(name)
            if chevron:
                dbg.click(t["cx"] + t["cw"] // 2, t["y"] + t["h"] // 2)
            else:
                dbg.click(t["x"] + t["w"] // 2, t["y"] + t["h"] // 2)
            dbg.settle()

        def open_card():
            b = q()["button"]
            dbg.click(b[0] + b[2] // 2, b[1] + b[3] // 2)
            return poll(lambda: (lambda s: s if s["open"] and s["w"] else None)(q()), 3) or q()

        st = open_card()
        res.check("the tray's network-volume button opens Quick Settings, six tiles",
                  st["open"] and st["w"] > 0 and len(st["tiles"]) == 6, f"quick: open {st['open']} w {st['w']}")
        net = tile("network")
        if net["on"]:
            qmp.screenshot(os.path.join(tmp, "card.png"))
            im = Image.open(os.path.join(tmp, "card.png")).convert("RGB")
            px = im.getpixel((net["x"] + 6, net["y"] + net["h"] // 2))
            res.check("the network tile, on, is drawn in the accent", near(px, (0x46, 0x6E, 0xA0)), f"pixel {px}")

        # --- tiles act through their owners -------------------------------
        click_tile("dnd")
        # 30 s, not 3: each read is `sh config get`, a guest program, and a
        # step left half done made the next tile's click land mid-change.
        ok = poll(lambda: "on" in cfg("desktop.notifications.dnd") and dbg.json("gui notify --json")["dnd"], 30)
        res.check("Do not disturb turns the setting and the bell on", bool(ok), cfg("desktop.notifications.dnd"))
        click_tile("dnd")
        poll(lambda: "off" in cfg("desktop.notifications.dnd"), 30)

        click_tile("transparency")
        ok = poll(lambda: "on" in cfg("desktop.transparency"), 30)
        res.check("Transparency turns desktop.transparency on", bool(ok), cfg("desktop.transparency"))
        click_tile("transparency")
        poll(lambda: "off" in cfg("desktop.transparency"), 3)

        click_tile("osk")
        on = poll(lambda: overlays()["osk"], 3)
        click_tile("osk")
        off = poll(lambda: not overlays()["osk"], 3)
        res.check("On-screen keyboard opens and closes it", bool(on) and bool(off), f"on {on} off {off}")

        # --- a hovered tile keeps its outline ------------------------------
        t, st = tile("dnd"), q()   # a whole tile: no chevron
        mid = t["y"] + t["h"] // 2
        rest_png, hover_png = dbg.hover_frames(qmp, tmp, rest_at=(st["x"] + 4, st["y"] + st["h"] - 4),
                                               hover_at=(t["x"] + t["w"] // 4, mid), prefix="tile")
        a = Image.open(rest_png).convert("RGB")
        b = Image.open(hover_png).convert("RGB")
        edge, inside = (t["x"] + t["w"] - 1, mid), (t["x"] + t["w"] - 4, mid)
        res.check("a hovered tile is washed inside", a.getpixel(inside) != b.getpixel(inside),
                  f"inside {a.getpixel(inside)} -> {b.getpixel(inside)}")
        res.check("a hovered tile keeps its right-hand outline", a.getpixel(edge) == b.getpixel(edge),
                  f"edge {a.getpixel(edge)} -> {b.getpixel(edge)}")

        # --- the slider ---------------------------------------------------
        s = q()["sliders"][0]
        if s["shown"] and s["w"]:
            dbg.click(s["x"] + s["w"] // 4, s["y"] + s["h"] // 2)
            want = s["min"] + (s["max"] - s["min"]) // 4
            ok = poll(lambda: abs(int((cfg("system.volume").split("=")[-1] or "-1").split()[0]) - want) <= 2, 3)
            res.check("a click a quarter along the volume slider sets system.volume to a quarter",
                      bool(ok), f"system.volume {cfg('system.volume')!r}, wanted ~{want}")
        else:
            res.skip("the volume slider", "no sound device in this guest")

        # --- the keyboard layout tile ------------------------------------
        dbg.send("sh config set system.keyboard.layouts us,fi")
        poll(lambda: not tile("layout")["disabled"], 5)
        before = cfg("system.keyboard.layout")
        click_tile("layout")
        after = poll(lambda: (lambda v: v if v != before else None)(cfg("system.keyboard.layout")), 3)
        res.check("the Keyboard tile switches to the next layout", bool(after), f"{before!r} -> {after!r}")

        # --- Back is in the page's header, not on its first row -----------
        def page_back(name, first_row_top):
            click_tile(name, chevron=True)
            st = poll(lambda: (lambda s: s if s["page"] == name else None)(q()), 3) or q()
            b = st["back"]
            top = first_row_top() if st["page"] == name else None
            res.check(f"the {name} page's Back sits above its first row",
                      top is not None and b[2] > 0 and b[1] + b[2] <= top,
                      f"page {st['page']!r}, back y {b[1]}..{b[1] + b[2]}, first row at {top}")
            dbg.click(b[0] + b[2] // 2, b[1] + b[2] // 2)
            poll(lambda: (lambda s: s if s["open"] and not s["page"] else None)(q()), 3)

        page_back("layout", lambda: dbg.json("gui layout --json")["rows"][0]["top"])
        # The remote tile's chevron is a page only while a session is live;
        # otherwise it opens Settings' Remote Desktop page and closes the card.
        if dbg.json("gui remote --json")["sessions"] > 0:
            page_back("remote", lambda: dbg.json("gui remote --json")["list_y"])
        else:
            res.skip("the remote page's Back", "no live remote session (its chevron opens Settings)")
        dbg.send("sh config set system.keyboard.layouts us")
        dbg.send("sh config set system.keyboard.layout us")

        # --- a page, Back, and out --------------------------------------
        click_tile("network", chevron=True)
        st = poll(lambda: (lambda s: s if s["page"] == "network" else None)(q()), 3) or q()
        res.check("the network chevron shows the network flyout as a page, with Back",
                  st["page"] == "network" and overlays()["network"] and st["back"][2] > 0, f"quick: {st['page']!r}")
        b = st["back"]
        dbg.click(b[0] + b[2] // 2, b[1] + b[2] // 2)
        st = poll(lambda: (lambda s: s if s["open"] and not s["page"] and s["w"] else None)(q()), 3) or q()
        res.check("Back returns to the tiles", st["open"] and not st["page"] and not overlays()["network"],
                  f"quick: page {st['page']!r}, network {overlays()['network']}")
        s = q()["sliders"][0]
        if s["shown"] and s["w"]:
            c = s["chevron"]
            dbg.click(c[0] + c[2] // 2, c[1] + c[2] // 2)
            poll(lambda: q()["page"] == "volume", 3)
        else:
            click_tile("network", chevron=True)
            poll(lambda: q()["page"], 3)
        dbg.click(200, 200)
        dbg.settle()
        st = q()
        res.check("a click outside closes the page and Quick Settings with it",
                  not st["open"] and not overlays()["volume"] and not overlays()["network"], f"quick: {st['open']}")

        st = open_card()
        dbg.key("0x1b")
        dbg.settle()
        res.check("Esc closes the card", not q()["open"], str(q()["open"]))
        dbg.send("sh config unset desktop.notifications.dnd")
        dbg.send("sh config unset desktop.transparency")

    return res.finish("quick_test")


if __name__ == "__main__":
    sys.exit(main())
