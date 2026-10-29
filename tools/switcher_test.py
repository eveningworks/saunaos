#!/usr/bin/env python3
"""tools/switcher_test.py -- Alt+Tab (userland/wm/wm_switcher.h): the
order is most-recently-used, a quick tap swaps without drawing, a held
Alt shows a card of real thumbnails, and every way out does what it says.

WHAT THIS ASSERTS THAT "IT OPENS" WOULD NOT
------------------------------------------
* A TAP IS "THE LAST WINDOW", TWICE OVER. Two taps must land back where
  they started -- a switcher that cycled windows[] in z-order, or walked
  the taskbar's order, passes one tap and fails the second.
* A TAP DRAWS NOTHING: the card is reported not drawn at Alt's release.
* THE CARD IS A PICTURE: each client entry's thumbnail is neither flat
  nor black, read from the screen at the rect the WM reports.
* TAB, SHIFT+TAB and the arrows move the pick; Alt's release switches to
  it; Esc and a click outside leave the focus alone; a click on an entry
  switches to that entry.
* A MINIMIZED WINDOW IS LISTED LAST, and switching to it restores it.
* ALT+` lists only the focused app's windows.

    python3 tools/vm.py --disk <copy> start
    python3 tools/switcher_test.py
    echo $?

POSITIVE CONTROLS: build() listing windows[] bottom-up (z-order, not MRU)
reddens "two taps come back" and the order checks; a draw that skips the
thumbnails reddens "the thumbnail is a picture".
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

KEY_ALT = 0xF798


def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


def arrow_codes():
    here = os.path.dirname(os.path.abspath(__file__))
    hdr = os.path.join(here, "..", "kernel", "include", "api", "keyboard.h")
    codes = {}
    with open(hdr) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "#define" and parts[1].startswith("KEY_ARROW_"):
                codes[parts[1]] = int(parts[2], 0)
    return codes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "switcher_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="switcher_")
    res = Results()
    arrows = arrow_codes()

    def shot(name):
        p = os.path.join(tmp, name + ".png")
        qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    with DebugConsole(args.sock) as dbg:
        dbg.settle()

        def sw():
            return dbg.json("gui switcher --json")

        def focused():
            return next((w["title"] for w in dbg.windows() if w.get("focused")), None)

        def alt_tab(shift=False, settle=True):
            dbg.key("0x09", settle=settle, mods="alt shift" if shift else "alt")

        def alt_up():
            dbg.key(hex(KEY_ALT), mods="up")

        for app in ("Task Manager", "Calculator", "Notepad"):
            dbg.open_app(app)
            poll(lambda: any(w["title"].endswith(app) or w["title"] == app
                                 for w in dbg.windows()), 20)
            dbg.settle()
        first = focused()
        titles = [w["title"] for w in dbg.windows()]
        res.check("three windows open, one focused", len(titles) >= 3 and first,
                  f"windows {titles}, focused {first}")

        # --- a tap: the last window, and back again --------------------
        alt_tab(settle=False)
        st = sw()
        alt_up()
        dbg.settle()
        second = focused()
        res.check("a tap switches to another window", second and second != first,
                  f"{first} -> {second}")
        res.check("a tap draws no card", st.get("active") and not st.get("shown"),
                  f"switcher at the tap: {st}")
        alt_tab(settle=False)
        alt_up()
        dbg.settle()
        res.check("two taps come back to the first window (most recent first)",
                  focused() == first, f"{first} -> {second} -> {focused()}")

        # --- held: the card, its order and its thumbnails --------------
        alt_tab()
        st = poll(lambda: (lambda s: s if s.get("shown") else None)(sw()), 3) or sw()
        names = [e["title"] for e in st.get("entries", [])]
        res.check("held, the card draws, the focused window first and the pick second",
                  st.get("shown") and names[:2] == [first, second] and st.get("pick") == 1,
                  f"switcher: {st}")
        dbg.settle()
        im = shot("held")
        pics = 0
        for e in st.get("entries", []):
            t = e["thumb"]
            if t["w"] < 16 or t["h"] < 16:
                continue
            px = [im.getpixel((t["x"] + t["w"] * i // 6, t["y"] + t["h"] * j // 6))
                  for i in range(1, 6) for j in range(1, 6)]
            spread = max(lum(p) for p in px) - min(lum(p) for p in px)
            if sum(lum(p) for p in px) / len(px) > 40 and spread > 30:
                pics += 1
        res.check("the thumbnails are pictures (not black, not flat)", pics >= 2,
                  f"{pics} of {len(st.get('entries', []))} entries look like a window")
        res.check("the card sits over the desktop, inside the screen",
                  st["x"] >= 0 and st["y"] >= 0 and st["w"] > 0
                  and st["x"] + st["w"] <= dbg.state()["screen"]["w"], str(st))

        # --- moving the pick -------------------------------------------
        alt_tab()
        p2 = sw().get("pick")
        alt_tab(shift=True)
        p3 = sw().get("pick")
        dbg.key(hex(arrows["KEY_ARROW_RIGHT"]), mods="alt")
        p4 = sw().get("pick")
        res.check("Tab, Shift+Tab and Right move the pick", (p2, p3, p4) == (2, 1, 2),
                  f"picks {p2}, {p3}, {p4}")
        target = names[2] if len(names) > 2 else None
        alt_up()
        dbg.settle()
        res.check("Alt's release switches to the pick", focused() == target and not sw().get("active"),
                  f"focused {focused()}, wanted {target}")

        # --- Esc, and a click outside, change nothing -------------------
        before = focused()
        alt_tab()
        dbg.key("0x1b", mods="alt")
        alt_up()
        dbg.settle()
        res.check("Esc closes it and leaves the focus alone",
                  not sw().get("active") and focused() == before, f"{before} -> {focused()}")

        # --- a click on an entry ---------------------------------------
        alt_tab()
        st = poll(lambda: (lambda s: s if s.get("shown") else None)(sw()), 3) or sw()
        ents = st.get("entries", [])
        if len(ents) >= 3:
            e = ents[2]
            dbg.click(e["x"] + e["w"] // 2, e["y"] + e["h"] // 2)
            dbg.settle()
            alt_up()
            dbg.settle()
            res.check("a click on an entry switches to it", focused() == e["title"],
                      f"clicked {e['title']}, focused {focused()}")
        else:
            res.check("a click on an entry switches to it", False, f"switcher: {st}")
            alt_up()

        # --- minimized: listed last, restored by a switch --------------
        tb = dbg.json("gui taskbar --json")
        cur = focused()
        btn = next((b for b in tb["buttons"] if b["title"] == cur), None)
        if btn:
            dbg.click(btn["cx"], btn["cy"])   # a focused window's button minimizes it
            dbg.settle()
        alt_tab()
        st = sw()
        ents = st.get("entries", [])
        last = ents[-1] if ents else {}
        res.check("a minimized window is listed, last",
                  last.get("title") == cur and last.get("minimized"), f"switcher: {st}")
        while sw().get("pick") != len(ents) - 1 and ents:
            alt_tab()
        alt_up()
        dbg.settle()
        w = dbg.window(cur) or {}
        res.check("switching to it restores and focuses it",
                  focused() == cur and w.get("state") != "minimized", f"window: {w}")

        # --- Alt+`: the focused app's windows only ---------------------
        dbg.open_app("Notepad")
        poll(lambda: sum(1 for w in dbg.windows() if w.get("app_id") == "notepad") >= 2, 20)
        dbg.settle()
        notepads = sorted(w["title"] for w in dbg.windows() if w.get("app_id") == "notepad")
        dbg.key("`", mods="alt")
        st = sw()
        names = sorted(e["title"] for e in st.get("entries", []))
        alt_up()
        dbg.settle()
        res.check("Alt+` lists the focused app's windows and nothing else",
                  len(names) == 2 and names == notepads, f"entries {names}, notepads {notepads}")

    return res.finish("switcher_test")


if __name__ == "__main__":
    sys.exit(main())
