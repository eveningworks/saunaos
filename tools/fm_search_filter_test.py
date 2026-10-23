#!/usr/bin/env python3
"""The File Manager's search filters (lib/ufind.h): the strip's type,
date and size chips and Contents (inside text files), asserted from what
the app reports -- its filter state, and the paths a finished search found.

The fixture /sf: dusk.jpg, empty.txt, notes.txt ("dusk" on line 4 only),
sky.bin (a GIF with "the dusk sky" appended -- binary, so never searched
inside), and a/ holding dusk-deep.gif and dusk-theme.mid.

1. **Ctrl+F opens the strip with its chips**, before anything is typed,
   and no filter is in force.
2. **Type > Pictures narrows the folder as it stands**: dusk.jpg alone
   (and '..'); the folder a/ and the text files are dropped.
3. **Enter with the filter alone searches below for it**: dusk.jpg and
   a/dusk-deep.gif, and nothing else -- no query was typed.
4. **Contents, then "dusk"**: the three names, plus notes.txt
   by its line 4 -- and NOT sky.bin, whose text a binary-blind search
   would find. The results gain a Match column.
5. **Size > Empty re-runs the search with it**: nothing is both empty
   and holds "dusk" -- the negative control, as a size filter ignored by
   the walk would list the same five again.
6. **Esc in the search box drops every filter** and the strip with them.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/sf"
K_CTRL_F = "0x06"
CHIP = {"type": 0, "date": 1, "size": 2, "inside": 3}
KIND_PICTURES, SIZE_EMPTY = 1, 1
# Each drop-down's rows, as files.c fills them: "Any ..." first.
ROW_ANY, ROW_PICTURES, ROW_EMPTY = 0, 1 + KIND_PICTURES, SIZE_EMPTY


def setup(dbg):
    dbg.send(f"sh rm -r {ROOT}")
    for d in (ROOT, f"{ROOT}/a"):
        dbg.send(f"sh mkdir {d}")
    dbg.write_lines(f"{ROOT}/dusk.jpg", ["not really a picture"])
    dbg.send(f"sh touch {ROOT}/empty.txt")
    dbg.write_lines(f"{ROOT}/notes.txt", ["shopping", "milk", "bread",
                                         "switch the wallpaper back to dusk"])
    dbg.send(f"sh cp /usr/share/pictures/swatches.gif {ROOT}/sky.bin")
    dbg.send(f"sh spawn /bin/tosh -c echo the dusk sky >> {ROOT}/sky.bin")
    dbg.send(f"sh cp /usr/share/pictures/swatches.gif {ROOT}/a/dusk-deep.gif")
    dbg.send(f"sh cp /usr/share/music/first-boot.mid {ROOT}/a/dusk-theme.mid")


def hits_since(mark):
    """The paths (with their line) the last finished search logged, after
    buffer index `mark` -- None while it has not finished."""
    buf = fm._ALL_BUF[mark:]
    done = [i for i, ln in enumerate(buf) if "files: search done" in ln]
    if not done:
        return None
    out, start = [], 0
    for i in range(done[-1] - 1, -1, -1):          # back to the previous done
        if "files: search done" in buf[i]:
            start = i + 1
            break
    for ln in buf[start:done[-1]]:
        if "files: search hit " in ln:
            w = ln.split("files: search hit ", 1)[1].split()
            if len(w) >= 2 and w[0].lstrip("-").isdigit():
                out.append((int(w[0]), w[1]))
    return sorted(out, key=lambda h: h[1])


def wait_hits(dbg, mark, timeout=25.0):
    end = time.time() + timeout
    while time.time() < end:
        fm._collect(dbg)
        h = hits_since(mark)
        if h is not None:
            return h
        time.sleep(0.3)
    return None


def settled_click(dbg, qmp, lay, r):
    dbg.warp_cursor(qmp, lay.ox + r[0] + r[2] // 2, lay.oy + r[1] + r[3] // 2)
    time.sleep(0.4)
    qmp.click()


def choose(dbg, qmp, win, chip, row, res, what):
    """A chip's drop-down, then its row -- each where the app says it is."""
    lay = fm.layout_now(dbg, win) or fm.last_layout()
    r = lay.filterbtn.get(CHIP[chip]) if lay else None
    if not r:
        res.check(f"{what}: the chip is reported", False, f"chips={lay and lay.filterbtn}")
        return
    pop = None
    for _ in range(2):   # one retry: a click can land while the window settles
        settled_click(dbg, qmp, lay, r)
        pop = fm.wait_layout(dbg, win, lambda l: row in l.popitems, timeout=6.0)
        if pop:
            break
    if not pop:
        res.check(f"{what}: its drop-down opens", False, f"popup={fm._popup_state(dbg, win)}")
        return
    settled_click(dbg, qmp, pop, pop.popitems[row])


def run(dbg, qmp, res):
    setup(dbg)
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT and l.rows.get(0, 0) >= 5)
    time.sleep(1.0)

    # 1.
    dbg.key(K_CTRL_F)
    lay = fm.wait_layout(dbg, win, lambda l: l.filter and l.filter[0] == 1 and len(l.filterbtn) == 4) \
        or fm.last_layout()
    res.check("Ctrl+F opens the strip with its four chips, nothing typed",
              lay is not None and lay.filter is not None and lay.filter[0] == 1 and len(lay.filterbtn) == 4,
              f"filter={lay and lay.filter} chips={lay and lay.filterbtn}")
    res.check("...and no filter is in force", lay is not None and lay.filter is not None
              and lay.filter[1:] == (-1, 0, 0, 0), f"filter={lay and lay.filter}")

    # 2.
    choose(dbg, qmp, win, "type", ROW_PICTURES, res, "Type > Pictures")
    lay = fm.wait_layout(dbg, win, lambda l: l.filter and l.filter[1] == KIND_PICTURES and l.rows.get(0) == 2) \
        or fm.last_layout()
    res.check("Type > Pictures leaves dusk.jpg alone in the folder (and '..')",
              lay is not None and lay.filter is not None and lay.filter[1] == KIND_PICTURES
              and lay.rows.get(0) == 2, f"filter={lay and lay.filter} rows={lay and lay.rows.get(0)}")

    # 3.
    mark = len(fm._ALL_BUF)
    dbg.key(K_CTRL_F)
    fm.wait_layout(dbg, win, lambda l: l.searching == 1, timeout=4.0)
    dbg.key(fm.K_ENTER)
    hits = wait_hits(dbg, mark)
    want = [(0, f"{ROOT}/a/dusk-deep.gif"), (0, f"{ROOT}/dusk.jpg")]
    res.check("Enter with the filter alone finds both pictures below, and only them",
              hits == want, f"hits={hits}")

    # 4. Back to any type (the results had nothing else to show, so the
    # folder comes back), then Contents, then the query.
    choose(dbg, qmp, win, "type", ROW_ANY, res, "Type > Any type")
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT and l.filter and l.filter[1] == -1) \
        or fm.last_layout()
    res.check("Any type with nothing typed goes back to the folder",
              lay is not None and lay.dir.get(0) == ROOT, f"dir={lay and lay.dir.get(0)}")
    r = lay.filterbtn.get(CHIP["inside"]) if lay else None
    if r:
        settled_click(dbg, qmp, lay, r)
    lay = fm.wait_layout(dbg, win, lambda l: l.filter and l.filter[4] == 1) or fm.last_layout()
    res.check("the Contents chip latches", lay is not None and lay.filter is not None
              and lay.filter[4] == 1, f"filter={lay and lay.filter}")
    mark = len(fm._ALL_BUF)
    dbg.key(K_CTRL_F)
    fm.wait_layout(dbg, win, lambda l: l.searching == 1, timeout=4.0)
    for ch in "dusk":
        dbg.key(ch)
    dbg.key(fm.K_ENTER)
    hits = wait_hits(dbg, mark)
    want = [(0, f"{ROOT}/a/dusk-deep.gif"), (0, f"{ROOT}/a/dusk-theme.mid"),
            (0, f"{ROOT}/dusk.jpg"), (4, f"{ROOT}/notes.txt")]
    res.check("'dusk' inside text files: the three names, and notes.txt by its line 4",
              hits == want, f"hits={hits}")
    res.check("...and not sky.bin, which is binary",
              hits is not None and not any(p.endswith("sky.bin") for _, p in hits), f"hits={hits}")
    # From the frames the search drew: the window is idle once the walk
    # is done, and a frame asked for now can be an older one still queued.
    heads = [ln.split("files: layout left.heads", 1)[1].strip().split("|")
             for ln in fm._ALL_BUF[mark:] if "files: layout left.heads" in ln]
    res.check("...and the results show a Match column, second after Name",
              bool(heads) and heads[-1][:2] == ["Name", "Match"], f"heads={heads[-1:] }")

    # 5.
    mark = len(fm._ALL_BUF)
    choose(dbg, qmp, win, "size", ROW_EMPTY, res, "Size > Empty")
    hits = wait_hits(dbg, mark)
    res.check("Size > Empty searches again and finds nothing both empty and holding 'dusk'",
              hits == [], f"hits={hits}")

    # 6.
    dbg.key(K_CTRL_F)
    fm.wait_layout(dbg, win, lambda l: l.searching == 1, timeout=4.0)
    dbg.key(fm.K_ESC)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT and l.filter
                         and l.filter == (0, -1, 0, 0, 0)) or fm.last_layout()
    res.check("Esc drops every filter, and the strip with them",
              lay is not None and lay.filter == (0, -1, 0, 0, 0) and lay.dir.get(0) == ROOT,
              f"filter={lay and lay.filter} dir={lay and lay.dir.get(0)}")
    dbg.send(f"sh rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_search_filter_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_search_filter_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
