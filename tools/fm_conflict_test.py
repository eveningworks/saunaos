#!/usr/bin/env python3
"""The File Manager's Replace-or-skip card (ui/uui_compare.h): two files of
one name, side by side, and what each answer does on disk -- read back
with `cat`, a different reader from the app that did the work.

The fixture is two conflicts in ONE copy, made so their dates disagree:
a.txt is NEWER and LARGER coming in; b.txt is newer where it already is.

1. **Copying both raises the card for a.txt** and leaves the
   destination untouched while it is up.
2. **The card compares them**: the incoming side carries both badges,
   newer and larger, and the existing side neither.
3. **Its "Do the same for the rest" box ticks** on a click.
4. **Keep newer, for the rest too**: a.txt is replaced (the incoming one
   is newer) and b.txt is kept (the one already there is) -- with no
   second card. The control: a Keep newer that replaced regardless, or
   a box that applied nothing, fails one of the two files.

Shares filemanager_test.py's layout reader; the fixture is /cf.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/cf"
K_CTRL_A = "0x01"
BTN = {"replace": 0, "newer": 1, "skip": 2, "both": 3, "cancel": 4}


def run(dbg, qmp, res):
    def cat(path):
        return (dbg.send(f"sh cat {path}") or "").strip()

    dbg.send(f"sh rm -r {ROOT}")
    for d in (ROOT, f"{ROOT}/src", f"{ROOT}/dst"):
        dbg.send(f"sh mkdir {d}")
    # Dates by ORDER OF WRITING, a couple of seconds apart (whole seconds
    # are what a file keeps): dst/a, then both sources, then dst/b.
    dbg.write_lines(f"{ROOT}/dst/a.txt", ["old a"])
    time.sleep(2.5)
    dbg.write_lines(f"{ROOT}/src/a.txt", ["new a, and longer than the old one"])
    dbg.write_lines(f"{ROOT}/src/b.txt", ["older b"])
    time.sleep(2.5)
    dbg.write_lines(f"{ROOT}/dst/b.txt", ["newest b, already there"])

    dbg.write_lines(fm.FILES_CONF, ["panes=2", "tree=0", "left_view=details",
                                    "right_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}/src {ROOT}/dst", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{ROOT}/src" and l.rows.get(0, 0) >= 2)
    time.sleep(0.5)

    # 1.
    dbg.key(K_CTRL_A, mods="ctrl")
    time.sleep(0.5)
    dbg.key(fm.K_F5)
    lay = fm.wait_layout(dbg, win, lambda l: l.dialog == 1 and "side0" in l.compare, timeout=15) \
        or fm.last_layout()
    res.check("copying both raises the card", lay is not None and lay.dialog == 1,
              f"dialog={lay and lay.dialog}")
    res.check("...and the destination is untouched while it is up",
              cat(f"{ROOT}/dst/a.txt") == "old a", repr(cat(f"{ROOT}/dst/a.txt")))
    if not lay or "side0" not in lay.compare:
        res.check("the card reports its two sides", False, f"compare={lay and lay.compare}")
        return

    # 2.
    s0 = lay.compare.get("side0", [""])[0].split("|")
    s1 = lay.compare.get("side1", [""])[0].split("|")
    res.check("the incoming side is newer and larger",
              s0[1:] == ["newer", "larger"], f"side0={s0}")
    res.check("...and the existing side is neither", s1[1:] == ["-", "-"], f"side1={s1}")

    # 3. The box's rect, as the card reports it.
    r = [int(v) for v in lay.compare.get("rest", [])[:4]]
    if len(r) == 4:
        fm.sure_click(dbg, qmp, lay.ox + r[0] + 6, lay.oy + r[1] + r[3] // 2)
    lay = fm.wait_layout(dbg, win, lambda l: l.compare.get("rest_checked") == ["1"]) or fm.last_layout()
    res.check("the 'Do the same for the rest' box ticks",
              lay is not None and lay.compare.get("rest_checked") == ["1"],
              f"rest={r} checked={lay and lay.compare.get('rest_checked')}")

    # 4. Keep newer: arrowed to, one press at a time, then Enter.
    hot = lay.dlghot if lay and lay.dlghot is not None else 0
    for _ in range(8):
        if hot == BTN["newer"]:
            break
        dbg.key(fm.K_RIGHT)
        got = fm.wait_layout(dbg, win, lambda l, w=(hot + 1) % 5: l.dlghot == w, timeout=4.0)
        if got and got.dlghot is not None:
            hot = got.dlghot
    dbg.key(fm.K_ENTER)
    deadline = time.time() + 15
    a = b = ""
    while time.time() < deadline:
        a, b = cat(f"{ROOT}/dst/a.txt"), cat(f"{ROOT}/dst/b.txt")
        if a.startswith("new a"):
            break
        time.sleep(0.5)
    time.sleep(1.5)
    b = cat(f"{ROOT}/dst/b.txt")
    # A FRESH frame with no card, waited for: the app is idle once the copy
    # ends, and the last frame logged is the one with the card up.
    nocard = fm.wait_layout(dbg, win, lambda l: l.dialog == 0, timeout=10)
    res.check("Keep newer replaces a.txt, the incoming one being newer",
              a.startswith("new a"), repr(a))
    res.check("...and keeps b.txt, the one already there being newer",
              b.startswith("newest b"), repr(b))
    res.check("...without a second card", nocard is not None,
              f"last dialog={fm.last_layout() and fm.last_layout().dialog}")
    dbg.send(f"sh rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_conflict_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_conflict_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
