#!/usr/bin/env python3
"""tools/snap_test.py -- window snapping (userland/wm/wm_snap.h): edge and
corner drops, Super+arrows, Snap Assist, Snap layouts on Maximize, and
the way back off a zone.

WHAT THIS ASSERTS THAT "IT MOVED" WOULD NOT
------------------------------------------
* THE RECT IS THE ZONE'S: a left snap starts at 0,0 and is the work
  area's height; a right one starts at half the screen.
* THE WAY BACK IS THE WINDOW'S OWN SIZE: dragged off its half, Super+
  arrowed off it, or maximized and restored, a snapped window lands at
  the width it had BEFORE the snap -- a restore that read the half's
  rect passes every "it moved" check and fails this one.
* SNAP ASSIST lists the others in the empty half, and a click there
  puts that window in it.
* SNAP LAYOUTS opens on a rest over Maximize, lights a zone and shows it
  on the screen (a wallpaper pixel in the zone turns toward the accent),
  and a click lays the window in that zone and offers the layout's next.
* A window that cannot resize (Calculator) does not snap.

    python3 tools/vm.py --disk <copy> start
    python3 tools/snap_test.py
    echo $?

POSITIVE CONTROLS: wm_snap_window() taking the restore rect on every
snap (not only the first) reddens the four "at its own size" checks
after a walk through the zones -- Super+Right back, Super+Down from
maximized, a restore after a maximize, a drag off the top -- and
nothing else; the preview drawn at alpha 0 reddens "the hovered zone
shows on the screen" alone.
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


def arrow_codes():
    here = os.path.dirname(os.path.abspath(__file__))
    hdr = os.path.join(here, "..", "kernel", "include", "api", "keyboard.h")
    codes = {}
    with open(hdr) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "#define" and parts[1].startswith("KEY_ARROW_"):
                codes[parts[1][len("KEY_ARROW_"):].lower()] = int(parts[2], 0)
    return codes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "snap_test")

    from PIL import Image
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    tmp = tempfile.mkdtemp(prefix="snap_")
    res = Results()
    arrow = arrow_codes()

    def shot(name):
        p = os.path.join(tmp, name + ".png")
        qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        scr = dbg.state()["screen"]
        sw_, sh_ = scr["w"], scr["h"]
        tb_y = dbg.json("gui taskbar --json")["y"]
        work_h = tb_y if 0 < tb_y < sh_ else sh_

        def win(title):
            return dbg.window(title) or {}

        def super_key(name):
            dbg.key(hex(arrow[name]), mods="super")
            dbg.settle()

        def sw():
            return dbg.json("gui switcher --json")

        def settled(title, fn, timeout=4):
            return poll(lambda: (lambda w: w if w and fn(w) else None)(win(title)), timeout) or win(title)

        for app in ("Task Manager", "Notepad"):
            dbg.open_app(app)
            dbg.settle()
        poll(lambda: win("Task Manager") and win("untitled"), 20)
        dbg.settle()
        np0 = win("untitled")
        tm0 = win("Task Manager")
        res.check("Notepad and Task Manager are open", np0 and tm0, str([w["title"] for w in dbg.windows()]))
        if not (np0 and tm0):
            return res.finish("snap_test")

        # --- a drag to the left edge, and Snap Assist -------------------
        dbg.drag(tm0["x"] + 120, tm0["y"] + 8, 0, work_h // 2, 16)
        tm = settled("Task Manager", lambda w: w["snap"] == "left")
        res.check("dragged to the left edge, Task Manager fills the left half",
                  tm.get("snap") == "left" and tm["x"] == 0 and tm["y"] == 0
                  and abs(tm["h"] - work_h) <= 2, f"window {tm.get('x')},{tm.get('y')} "
                  f"{tm.get('w')}x{tm.get('h')} snap {tm.get('snap')!r}, work area height {work_h}")
        st = poll(lambda: (lambda s: s if s.get("assist") else None)(sw()), 3) or sw()
        names = [e["title"] for e in st.get("entries", [])]
        res.check("Snap Assist offers the other windows in the right half",
                  st.get("assist") == "right" and names == ["untitled"] and st["x"] >= sw_ // 2 - 8,
                  f"switcher: {st}")
        if st.get("entries"):
            e = st["entries"][0]
            dbg.click(e["x"] + e["w"] // 2, e["y"] + e["h"] // 2)
            dbg.settle()
        np = settled("untitled", lambda w: w["snap"] == "right")
        res.check("a click there puts Notepad in the right half",
                  np.get("snap") == "right" and abs(np["x"] - sw_ // 2) <= 1 and not sw().get("assist"),
                  f"Notepad {np.get('x')},{np.get('y')} {np.get('w')}x{np.get('h')} snap {np.get('snap')!r}")

        # --- off its zone at its own width ------------------------------
        dbg.drag(np["x"] + 160, np["y"] + 8, sw_ // 2, work_h // 3, 16)
        np = settled("untitled", lambda w: w["snap"] == "" and w["w"] == np0["w"])
        res.check("dragged off its half, Notepad is back at its own width",
                  np.get("snap") == "" and np.get("w") == np0["w"] and np.get("h") == np0["h"],
                  f"{np.get('w')}x{np.get('h')} against {np0['w']}x{np0['h']}")

        # --- Super+arrows -----------------------------------------------
        seq = []
        for k in ("left", "up", "down", "down"):
            super_key(k)
            if k == "left":
                dbg.key("0x1b")      # the left half's Snap Assist: not now
                dbg.settle()
            seq.append(settled("untitled", lambda w: True).get("snap"))
        res.check("Super+Left, Up, Down, Down walk left half -> top quarter -> half -> bottom quarter",
                  seq == ["left", "tl", "left", "bl"], f"zones {seq}")
        super_key("right")
        np = settled("untitled", lambda w: w["snap"] == "")
        res.check("Super+Right from the left side comes back at its own size",
                  np.get("snap") == "" and np.get("w") == np0["w"], f"{np.get('w')} vs {np0['w']}, snap {np.get('snap')!r}")
        super_key("up")
        np = settled("untitled", lambda w: w["state"] == "maximized")
        super_key("down")
        np2 = settled("untitled", lambda w: w["state"] == "normal")
        res.check("Super+Up maximizes and Super+Down restores",
                  np.get("state") == "maximized" and np2.get("state") == "normal" and np2.get("w") == np0["w"],
                  f"{np.get('state')} -> {np2.get('state')} at {np2.get('w')}")

        # --- maximized from a half, then restored -----------------------
        super_key("left")
        dbg.key("0x1b")
        dbg.settle()
        super_key("up")         # left -> top-left quarter
        super_key("down")       # -> left half again
        np = win("untitled")
        band = (np["content"]["y"] - np["y"]) // 2
        tx, ty = np["x"] + 200, np["y"] + band
        dbg.click(tx, ty, settle=False)
        dbg.click(tx, ty)       # a double-click on the title maximizes
        mx = settled("untitled", lambda w: w["state"] == "maximized")
        np = win("untitled")
        dbg.click(np["x"] + 200, np["y"] + band, settle=False)
        dbg.click(np["x"] + 200, np["y"] + band)
        back = settled("untitled", lambda w: w["state"] == "normal")
        res.check("a snapped window maximized and restored lands at its own size, not the half",
                  mx.get("state") == "maximized" and back.get("snap") == "" and back.get("w") == np0["w"],
                  f"{mx.get('state')} -> {back.get('state')} {back.get('w')}x{back.get('h')} snap {back.get('snap')!r}")

        # --- the top edge maximizes; a drag down comes off it ------------
        np = win("untitled")
        dbg.drag(np["x"] + 160, np["y"] + band, sw_ // 2, 0, 16)
        mx = settled("untitled", lambda w: w["state"] == "maximized")
        res.check("dragged to the top edge, Notepad maximizes", mx.get("state") == "maximized", str(mx.get("state")))
        dbg.drag(sw_ // 2, band, sw_ // 2, work_h // 2, 16)
        off = settled("untitled", lambda w: w["state"] == "normal")
        res.check("dragged down off the top, it is back at its own size under the pointer",
                  off.get("state") == "normal" and off.get("w") == np0["w"]
                  and off["x"] < sw_ // 2 < off["x"] + off["w"],
                  f"{off.get('state')} at {off.get('x')} {off.get('w')}x{off.get('h')}")

        # --- a corner ---------------------------------------------------
        np = win("untitled")
        dbg.drag(np["x"] + 160, np["y"] + band, sw_ - 1, work_h - 2, 16)
        np = settled("untitled", lambda w: w["snap"] == "br")
        res.check("dragged into the bottom-right corner, it takes that quarter",
                  np.get("snap") == "br" and abs(np["x"] - sw_ // 2) <= 1 and abs(np["y"] - work_h // 2) <= 1,
                  f"snap {np.get('snap')!r} at {np.get('x')},{np.get('y')}")
        dbg.key("0x1b")
        dbg.settle()

        # --- a window that cannot resize does not snap -----------------
        dbg.open_app("Calculator")
        calc = poll(lambda: win("Calculator"), 20) or {}
        dbg.settle()
        if calc:
            dbg.drag(calc["x"] + 40, calc["y"] + 8, 0, work_h // 2, 16)
            dbg.settle()
            c = win("Calculator")
            res.check("Calculator, which cannot resize, does not snap", c.get("snap") == "", f"snap {c.get('snap')!r}")
        else:
            res.check("Calculator opens", False)

        # --- Snap layouts on Maximize -----------------------------------
        np = win("untitled")
        dbg.click(np["x"] + 200, np["y"] + band)   # focus and raise Notepad
        dbg.settle()
        np = win("untitled")
        maxb = None
        for x in range(np["x"] + np["w"] - 2, np["x"], -2):
            if dbg.probe(x, np["y"] + band).get("region") == "maximize-button":
                maxb = (x - 3, np["y"] + band)
                break
        res.check("Notepad has a Maximize button", maxb is not None)
        if maxb:
            dbg.warp_cursor(qmp, sw_ // 2, work_h - 40)
            dbg.settle()          # off every button, so the rest on Maximize starts fresh
            dbg.warp_cursor(qmp, *maxb)
            sl = poll(lambda: (lambda s: s if s.get("open") else None)(dbg.json("gui snaplayout --json")), 4) \
                or dbg.json("gui snaplayout --json")
            res.check("resting on Maximize opens Snap layouts with six layouts",
                      sl.get("open") and len({z["layout"] for z in sl.get("zones", [])}) == 6, f"snaplayout: {sl}")
            mid = next((z for z in sl.get("zones", []) if z["zone"] == "2-4,0-6"), None)
            if mid:
                # A wallpaper pixel in the middle third, before and while lit.
                px = (sw_ // 2, work_h - 12)
                before = shot("layouts").getpixel(px)
                dbg.warp_cursor(qmp, mid["x"] + mid["w"] // 2, mid["y"] + mid["h"] // 2)
                lit = poll(lambda: (lambda s: s if s.get("hot", -1) >= 0 else None)(dbg.json("gui snaplayout --json")), 3)
                dbg.settle()
                after = shot("layouts-hot").getpixel(px)
                blue_shift = (after[2] - after[0]) - (before[2] - before[0])
                res.check("the hovered zone lights, and shows on the screen in the accent",
                          bool(lit) and blue_shift > 8 and after != before,
                          f"hot {lit and lit.get('hot')}, pixel {before} -> {after}")
                dbg.click(mid["x"] + mid["w"] // 2, mid["y"] + mid["h"] // 2)
                dbg.settle()
                np = settled("untitled", lambda w: w["snap"] == "2-4,0-6")
                st = poll(lambda: (lambda s: s if s.get("assist") else None)(sw()), 3) or sw()
                res.check("a click lays Notepad in the middle third, and Snap Assist offers the left third",
                          np.get("snap") == "2-4,0-6" and st.get("assist") == "0-2,0-6"
                          and "Task Manager" in [e["title"] for e in st.get("entries", [])],
                          f"Notepad {np.get('snap')!r}, switcher {st}")
                tme = next((e for e in st.get("entries", []) if e["title"] == "Task Manager"), None)
                if tme:
                    dbg.click(tme["x"] + tme["w"] // 2, tme["y"] + tme["h"] // 2)
                    dbg.settle()
                    tm = settled("Task Manager", lambda w: w["snap"] == "0-2,0-6")
                    st = sw()
                    res.check("...a click puts Task Manager there, and the right third is offered next",
                              tm.get("snap") == "0-2,0-6" and st.get("assist") == "4-6,0-6",
                              f"Task Manager {tm.get('snap')!r}, switcher assist {st.get('assist')!r}")
                    dbg.key("0x1b")
                    dbg.settle()
            else:
                res.check("the thirds layout has a middle zone", False, str(sl))

        # --- Super+Z ------------------------------------------------------
        dbg.warp_cursor(qmp, sw_ // 2, work_h - 40)
        dbg.key("z", mods="super")
        sl = dbg.json("gui snaplayout --json")
        dbg.key("0x1b")
        dbg.settle()
        res.check("Super+Z opens Snap layouts with a zone lit, and Esc closes it",
                  sl.get("open") and sl.get("hot") == 0 and not dbg.json("gui snaplayout --json").get("open"),
                  f"snaplayout: {sl}")

    return res.finish("snap_test")


if __name__ == "__main__":
    sys.exit(main())
