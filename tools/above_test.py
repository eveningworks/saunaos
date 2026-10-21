"""tools/above_test.py -- Always on Top: stacked over the rest, focus unchanged.

Opens Notepad, then Calculator over it, and pins Calculator with
Ctrl+Super+T. Then asserts:

  * `gui windows` says Calculator is `above` and STACKED over Notepad;
  * a click on Notepad, where Calculator does not cover it, FOCUSES
    Notepad -- and Calculator still stacks above it: the overlap's pixels
    are the same as while Calculator had focus, and `gui probe` in the
    overlap names Calculator (input follows the stacking, not focus);
  * the taskbar button carries the pin, and its pixels are the pin's
    amber, not the button's ground;
  * the window menu shows "Always on Top" TICKED, and choosing it unpins
    Calculator: the focused Notepad then covers the overlap, and the
    overlap's pixels CHANGE -- the check that the comparison above could
    have failed at all.

What a broken version would still pass: none of the stacking checks --
a window drawn in activation order puts Notepad over the overlap the
moment it is clicked, which the same-pixels check sees, and a hit test
left in activation order names Notepad in the probe.

    python3 tools/above_test.py [--instance N] [--in-gui] [--keep DIR]
"""
import argparse
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

AMBER = [(0xFF, 0xD1, 0x66), (0xB0, 0x78, 0x00)]   # draw_tb_pin(): dark, light strip


def find(dbg, icon):
    for w in dbg.windows():
        if (w.get("icon") or {}).get("name") == icon and not w.get("popup") \
                and not w.get("dialog"):
            return w
    return None


def close_all(dbg):
    for _ in range(12):
        ws = [w for w in dbg.windows() if not w.get("popup") and not w.get("dialog")]
        if not ws:
            return
        dbg.send(f"gui close {ws[-1]['z']}")
        time.sleep(0.8)


def open_win(dbg, name, icon):
    dbg.open_app(name)
    dbg.settle(1.0)
    return find(dbg, icon)


def overlap(a, b):
    x0, y0 = max(a["x"], b["x"]), max(a["y"], b["y"])
    x1 = min(a["x"] + a["w"], b["x"] + b["w"])
    y1 = min(a["y"] + a["h"], b["y"] + b["h"])
    return (x0, y0, x1, y1) if x1 > x0 and y1 > y0 else None


def outside(a, b):
    """A point of window a's content that window b does not cover --
    from the BOTTOM RIGHT, clear of a menu bar a click would open."""
    cx, cy, cw, ch = a["content"]["x"], a["content"]["y"], a["content"]["w"], a["content"]["h"]
    for y in range(cy + ch - 24, cy + 24, -8):
        for x in range(cx + cw - 24, cx + 24, -8):
            if not (b["x"] <= x < b["x"] + b["w"] and b["y"] <= y < b["y"] + b["h"]):
                return x, y
    return None


def crop(qmp, tmp, name, box):
    from PIL import Image
    path = f"{tmp}/{name}.png"
    qmp.screenshot(path)
    return Image.open(path).convert("RGB").crop(box)


def run(dbg, qmp, res, tmp):
    close_all(dbg)
    np = open_win(dbg, "Notepad", "notepad")
    calc = open_win(dbg, "Calculator", "calculator")
    if not np or not calc:
        res.check("Notepad and Calculator opened", False, f"windows: {dbg.windows()}")
        return
    # The overlap, kept INSIDE Calculator's content: its title bar and
    # frame edge change colour with focus, and that is not what is compared.
    cc = calc["content"]
    box = overlap(np, {"x": cc["x"] + 4, "y": cc["y"] + 4, "w": cc["w"] - 8, "h": cc["h"] - 8})
    res.check("Calculator opens over part of Notepad", box is not None,
              f"notepad {np['x']},{np['y']} {np['w']}x{np['h']}, calculator content {cc}")
    if not box:
        return
    np_pt = outside(np, calc)
    res.check("...and leaves part of Notepad uncovered", np_pt is not None, "fully covered")
    if not np_pt:
        return
    park = (dbg.state()["screen"]["w"] - 2, 2)

    dbg.logs("always on top", clear=True)
    dbg.key("t", mods="ctrl super")
    calc = find(dbg, "calculator")
    res.check("Ctrl+Super+T pins the focused Calculator", calc.get("above") is True,
              f"above {calc.get('above')!r}")
    res.check("...and the WM logs it", any("now always on top" in ln
                                          for ln in dbg.logs("always on top", clear=True)),
              "no 'now always on top' line")
    dbg.move(*park)
    before = crop(qmp, tmp, "pinned_focused", box)

    dbg.click(*np_pt)
    dbg.settle(1.0)
    dbg.move(*park)
    np, calc = find(dbg, "notepad"), find(dbg, "calculator")
    res.check("a click on Notepad focuses it", np.get("focused") is True,
              f"notepad focused {np.get('focused')!r}")
    res.check("...and Calculator still STACKS above it", calc["stack"] > np["stack"],
              f"stack calculator {calc['stack']} notepad {np['stack']}")
    after = crop(qmp, tmp, "pinned_unfocused", box)
    res.check("...and still covers the overlap, pixel for pixel",
              before.tobytes() == after.tobytes(), "the overlap changed")
    px, py = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
    hit = dbg.json(f"gui probe {px} {py} --json")
    res.check("...and a press in the overlap reaches Calculator",
              hit.get("title") == calc["title"], f"probe {hit}")

    b = next((x for x in dbg.taskbar()["buttons"] if x["title"] == calc["title"]), None)
    pin = (b or {}).get("pin")
    res.check("Calculator's taskbar button has the pin", pin is not None, f"button {b}")
    if pin:
        from PIL import Image
        path = f"{tmp}/taskbar.png"
        qmp.screenshot(path)
        im = Image.open(path).convert("RGB")
        s = pin["size"]
        amber = sum(1 for yy in range(pin["y"], pin["y"] + s) for xx in range(pin["x"], pin["x"] + s)
                    if any(all(abs(c - k) <= 40 for c, k in zip(im.getpixel((xx, yy)), a))
                           for a in AMBER))
        res.check("...drawn in amber", amber >= s, f"{amber} amber px in the {s}px box")
    nb = next((x for x in dbg.taskbar()["buttons"] if x["title"] == np["title"]), None)
    res.check("...and Notepad's has none", nb is not None and nb.get("pin") is None, f"{nb}")

    dbg.rclick(calc["x"] + calc["w"] // 3, calc["y"] + 8)
    time.sleep(0.6)
    rows = {r["label"]: r for r in dbg.ctxmenu().get("rows", [])}
    row = rows.get("Always on Top")
    res.check("the window menu shows Always on Top, ticked",
              row is not None and row.get("checked") is True, f"rows {list(rows)}")
    if not row:
        return
    dbg.click(row["cx"], row["cy"])
    dbg.settle(1.0)
    dbg.move(*park)
    calc = find(dbg, "calculator")
    res.check("...and choosing it unpins Calculator", calc.get("above") is False,
              f"above {calc.get('above')!r}")
    unpinned = crop(qmp, tmp, "unpinned", box)
    res.check("...and the focused Notepad now covers the overlap (the control)",
              unpinned.tobytes() != before.tobytes(), "the overlap did not change")
    close_all(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true", help="the VM already shows the desktop")
    ap.add_argument("--keep", metavar="DIR", help="keep the screenshots in DIR")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "above_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    print("above_test: checks")
    try:
        if args.keep:
            run(dbg, qmp, res, args.keep)
        else:
            with tempfile.TemporaryDirectory() as tmp:
                run(dbg, qmp, res, tmp)
    finally:
        dbg.close()
        qmp.close()
    print(f"\nabove_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
