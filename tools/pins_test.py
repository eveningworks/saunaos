#!/usr/bin/env python3
"""tools/pins_test.py -- pinned taskbar apps (userland/wm/wm_taskbar.h,
start_store.h's `taskbar` list): a pin is its icon until the app runs,
then the app's window button stands in its place; unpinned windows come
after the pins; the menus pin and unpin; a drag reorders the pins and
the order is kept in /etc/start-menu.conf.

WHAT THIS ASSERTS THAT "THE ICON IS THERE" WOULD NOT
---------------------------------------------------
* A PIN IS DRAWN: the pixel at its centre is not the strip's colour.
* THE WINDOW TAKES THE PIN'S PLACE -- the same x the pin had, flagged
  `pinned` -- and no pin button is left over beside it.
* AN UNPINNED WINDOW STAYS AFTER THE PINS, even dragged to the front:
  a drop the next layout would undo is refused while dragging.
* THE ORDER IS THE FILE'S: after a drag and a menu pin, the `taskbar`
  line in /etc/start-menu.conf names the pins in the strip's order.
* CLOSING THE APP GIVES THE PIN BACK, where it was.

    python3 tools/vm.py --disk <copy> start
    python3 tools/pins_test.py
    echo $?

POSITIVE CONTROL: resolve_pins() never marking a pin running (the pin
and its window both shown) reddens "the window stands in its pin's
place" and "its window now a pinned button"; apply_drag() without its
side clamp reddens "... while held" alone -- the drop hides it, since
the next layout puts the window back. Leaves the shipped pins.
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

DEFAULTS = ["files", "browser", "terminal"]


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "pins_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="pins_")
    res = Results()

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        for k in ("taskbar.buttons", "taskbar.combine", "taskbar.align", "taskbar.peek"):
            dbg.send(f"sh config unset desktop.{k}")
        # Peek off: its card would cover the strip while the pointer rests.
        dbg.send("sh config set desktop.taskbar.peek off")
        for _ in range(12):
            left = [w for w in dbg.windows() if not w.get("popup") and not w.get("dialog")]
            if not left:
                break
            dbg.send(f"gui close {left[-1]['z']}")
            if not poll(lambda: len(dbg.windows()) < len(left), 5):
                break
        dbg.send("gui pins set " + " ".join(DEFAULTS))
        dbg.settle()

        def tb():
            return dbg.json("gui taskbar --json")

        def pins():
            return tb()["pins"]

        def pin(app_id):
            return next((p for p in pins() if p["app_id"] == app_id), None)

        def order():
            return [p["app_id"] for p in pins()]

        def conf_order():
            out = dbg.send("sh cat /etc/start-menu.conf") or ""
            for line in out.splitlines():
                line = line.strip()
                if line.startswith("taskbar"):
                    return line.split("=", 1)[1].split()
            return None

        def win_button(app_id):
            return next((b for b in tb()["buttons"] if b["app_id"] == app_id), None)

        # --- the pins, with nothing running -------------------------------
        t = tb()
        ps = t["pins"]
        res.check("the shipped pins stand in order, none running",
                  [p["app_id"] for p in ps] == DEFAULTS and all(p["shown"] and not p["running"] for p in ps),
                  f"pins {ps}")
        res.check("...as icon-wide buttons, the first after Start",
                  all(p["w"] <= t["btn_h"] + 12 for p in ps) and ps[0]["x"] < t["start"]["x"] + t["start"]["w"] + 12,
                  f"widths {[p['w'] for p in ps]}, first x {ps[0]['x']}")
        shot = os.path.join(tmp, "pins.png")
        qmp.screenshot(shot)
        im = Image.open(shot).convert("RGB")
        px = [im.getpixel((p["cx"], p["cy"])) for p in ps]
        res.check("each pin's icon is drawn", all(c != rgb(t["bar"]) for c in px), f"centres {px}, bar {rgb(t['bar'])}")

        # --- a click starts the app, whose window takes the pin's place ---
        files_x = ps[0]["x"]
        dbg.click(ps[0]["cx"], ps[0]["cy"])
        b = poll(lambda: win_button("files"), 15)
        p = pin("files")
        res.check("a click on a pin starts its app", b is not None, f"buttons {[x['app_id'] for x in tb()['buttons']]}")
        res.check("the window stands in its pin's place, and the pin is gone",
                  b is not None and b["pinned"] and b["x"] == files_x and p and p["running"]
                  and p["x"] == files_x and len([q for q in pins() if not q["running"]]) == 2,
                  f"button {b and (b['x'], b['pinned'])}, pin {p}, pin x was {files_x}")

        # --- an unpinned window comes after every pin ---------------------
        dbg.open_app("Notepad")
        nb = poll(lambda: win_button("notepad"), 15)
        last = pin("terminal")
        res.check("an unpinned window comes after the pins",
                  nb is not None and not nb["pinned"] and last and nb["x"] > last["x"],
                  f"notepad {nb and nb['x']}, terminal pin {last and last['x']}")

        # --- a drag reorders the pins ------------------------------------
        term, first = pin("terminal"), pins()[0]
        dbg.drag(term["cx"], term["cy"], first["x"] + 2, first["cy"], steps=24)
        got = poll(lambda: order()[0] == "terminal" and order(), 3) or order()
        res.check("dragging the Terminal pin to the front reorders the pins",
                  got == ["terminal", "files", "browser"], f"order {got}")
        res.check("...and the order is written to /etc/start-menu.conf",
                  conf_order() == got, f"conf {conf_order()}")

        # HELD, not dropped: the layout puts every unpinned window back
        # after the pins on the drop anyway, so only the held button can
        # show whether the drag kept to its side.
        nb = win_button("notepad")
        front = tb()["start"]["x"] + tb()["start"]["w"] + 4
        last_pin = max(p["x"] + p["w"] for p in pins())
        dbg.warp_cursor(qmp, nb["cx"], nb["cy"])
        qmp.mouse_down()
        dbg.warp_cursor(qmp, nb["cx"] - 40, nb["cy"])
        dbg.warp_cursor(qmp, front, nb["cy"])
        held = poll(lambda: (lambda t: next((b for b in t["buttons"] if b["dragging"]), None)
                             if t["dragging"] else None)(tb()), 3)
        qmp.mouse_up()
        dbg.settle()
        res.check("an unpinned window dragged to the front stays after the pins, while held",
                  held is not None and held["app_id"] == "notepad" and held["x"] >= last_pin,
                  f"held {held and (held['app_id'], held['x'])}, pins end at {last_pin}")
        nb2 = win_button("notepad")
        res.check("...and after the drop, with the pins' order untouched",
                  nb2 and nb2["x"] >= last_pin and order() == got,
                  f"notepad x {nb2 and nb2['x']}, order {order()}")

        # --- the menus ---------------------------------------------------
        nb = win_button("notepad")
        dbg.rclick(nb["cx"], nb["cy"])
        row = poll(lambda: dbg.ctxmenu_row("Pin to taskbar"), 3)
        res.check("a window's button menu offers Pin to taskbar", row is not None, str(dbg.ctxmenu().get("rows")))
        if row:
            dbg.click(*row)
            ok = poll(lambda: order() == got + ["notepad"], 3)
            nb = win_button("notepad")
            res.check("...which pins it at the end, its window now a pinned button",
                      bool(ok) and nb and nb["pinned"], f"order {order()}, notepad {nb}")

        br = pin("browser")
        dbg.rclick(br["cx"], br["cy"])
        row = poll(lambda: dbg.ctxmenu_row("Unpin from taskbar"), 3)
        res.check("a pin's menu offers Unpin from taskbar", row is not None, str(dbg.ctxmenu().get("rows")))
        if row:
            dbg.click(*row)
            ok = poll(lambda: "browser" not in order(), 3)
            res.check("...which takes the pin away, and from the file",
                      bool(ok) and "browser" not in (conf_order() or ["browser"]), f"order {order()}, conf {conf_order()}")

        # --- closing the app gives the pin back, where it was -------------
        at = pin("files")["x"]
        w = next((w for w in dbg.windows() if w.get("app_id") == "files" or w["title"] == "File Manager"), None)
        if w:
            dbg.send(f"gui close {w['z']}")
        p = poll(lambda: (lambda q: q if q and not q["running"] else None)(pin("files")), 8) or pin("files")
        res.check("closing the app gives its pin back, in its place",
                  p and not p["running"] and p["x"] == at, f"pin {p}, was at {at}")

        dbg.send("gui pins set " + " ".join(DEFAULTS))
        dbg.send("sh config unset desktop.taskbar.peek")

    return res.finish("pins_test")


if __name__ == "__main__":
    sys.exit(main())
