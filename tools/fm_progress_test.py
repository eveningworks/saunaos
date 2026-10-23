#!/usr/bin/env python3
"""The File Manager's Operations panel (ui/uui_transfers.h over fm_jobs.c):
a copy's progress in bytes for the WHOLE operation, Pause and Resume, and
the operations waiting behind it -- asserted from the panel's own report
(`ops.state <row> <state> <got> <total> <rate>`) and from the disk.

The fixture is /pg/src: big/ holding COPIES of first-boot.mpg (tens of
MB, so a copy lasts long enough to pause under TCG), small.txt and
other.txt; the destination /pg/dst starts empty.

1. **F5 on big/ shows one running row whose total is every byte in it**
   -- the counting pass, exact, not one file's size.
2. **It moves**: the bytes done grow, and a rate is measured.
3. **Pause holds it**: the row says paused, and the bytes done do not move
   for over a second -- the control a Pause that only relabelled would fail.
4. **Two more copies wait behind it** (small.txt, other.txt), each a
   waiting row; **Cancel on other.txt's row takes it out**.
5. **Resume goes on** from where it stopped.
6. **Everything ends**: the panel goes, big/'s copies are whole on disk,
   small.txt was copied after it -- and other.txt, cancelled while it
   waited, was not.
"""
import argparse
import re
import sys
import time

import filemanager_test as fm
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/pg"
VIDEO = "/usr/share/videos/first-boot.mpg"
COPIES = 6
RUNNING, PAUSED, QUEUED = 0, 4, 5   # enum uui_xfer_state
EV_PAUSE, EV_RESUME = 4, 5          # UUI_XFER_EV_*


def size_of(dbg, path):
    """`ls -l`'s size column, or None."""
    out = dbg.send(f"sh ls -l {path}") or ""
    m = re.search(r"^-\s+(\d+)\s", out, re.M)
    return int(m.group(1)) if m else None


def setup(dbg):
    dbg.send(f"sh rm -r {ROOT}")
    for d in (ROOT, f"{ROOT}/src", f"{ROOT}/src/big", f"{ROOT}/dst"):
        dbg.send(f"sh mkdir {d}")
    for i in range(COPIES):
        dbg.send(f"sh cp {VIDEO} {ROOT}/src/big/v{i}.mpg")
    dbg.write_lines(f"{ROOT}/src/small.txt", ["small but whole"])
    dbg.write_lines(f"{ROOT}/src/other.txt", ["never copied"])


def select(dbg, win, name):
    """Type-ahead to `name` in the active pane, as a person picks a row."""
    dbg.key(fm.K_HOME)
    for ch in name:
        dbg.key(ch)
    return fm.wait_layout(dbg, win, lambda l: l.selected == name, timeout=6.0)


def click(dbg, qmp, lay, r):
    dbg.warp_cursor(qmp, lay.ox + r[0] + r[2] // 2, lay.oy + r[1] + r[3] // 2)
    time.sleep(0.3)
    qmp.click()


def run(dbg, qmp, res):
    setup(dbg)
    one = size_of(dbg, VIDEO)
    res.check("the fixture's video has a size", one is not None, "ls -l said nothing")
    if one is None:
        return
    want_total = one * COPIES
    dbg.write_lines(fm.FILES_CONF, ["panes=2", "tree=0", "left_view=details",
                                    "right_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}/src {ROOT}/dst", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == f"{ROOT}/src" and l.rows.get(0, 0) >= 4)
    time.sleep(0.5)

    # 1.
    res.check("big/ is selected", select(dbg, win, "big") is not None, "type-ahead missed")
    dbg.key(fm.K_F5)
    lay = fm.wait_layout(dbg, win, lambda l: 0 in l.ops and l.ops[0][0] == RUNNING
                         and l.ops[0][2] > 0 and l.ops[0][1] > 0, timeout=20.0) or fm.last_layout()
    row = lay.ops.get(0) if lay else None
    res.check("F5 shows a running row whose total is every byte in big/",
              row is not None and row[2] == want_total, f"row={row} want total {want_total}")

    # 2.
    lay2 = fm.wait_layout(dbg, win, lambda l: 0 in l.ops and row and l.ops[0][1] > row[1]
                          and l.ops[0][3] > 0, timeout=10.0) or fm.last_layout()
    row2 = lay2.ops.get(0) if lay2 else None
    res.check("it moves: the bytes done grow, and a rate is measured",
              row2 is not None and row is not None and row2[1] > row[1] and row2[3] > 0,
              f"before={row} after={row2}")

    # 3.
    lay = fm.layout_now(dbg, win) or lay2
    act = lay.opsact2.get(0) if lay else None
    if act and act[0] == EV_PAUSE:
        click(dbg, qmp, lay, act[1])
    lay = fm.wait_layout(dbg, win, lambda l: 0 in l.ops and l.ops[0][0] == PAUSED, timeout=6.0) \
        or fm.last_layout()
    paused = lay.ops.get(0) if lay else None
    res.check("Pause: the row says paused",
              paused is not None and paused[0] == PAUSED, f"pause={act} row={paused}")
    # PAUSE HOLDS AT THE NEXT CHUNK: the one being written when it was
    # pressed still lands (64 KiB, ufileop's buffer). Measured from a frame
    # taken after that, the bytes must not move at all.
    time.sleep(0.5)
    settled = fm.layout_now(dbg, win) or fm.last_layout()
    if settled and settled.ops.get(0, (None,))[0] == PAUSED:
        paused = settled.ops[0]
    time.sleep(1.5)
    later = fm.layout_now(dbg, win) or fm.last_layout()
    held = later.ops.get(0) if later else None
    res.check("...and its bytes do not move for over a second",
              paused is not None and held is not None and held[0] == PAUSED and held[1] == paused[1],
              f"at pause={paused} 1.5 s on={held}")
    if paused is None or paused[1] >= want_total:
        res.check("(the copy was still under way when paused)", False,
                  "it finished first: raise COPIES")

    # 4. Two more, behind it; the second taken out again.
    for name in ("small.txt", "other.txt"):
        select(dbg, win, name)
        dbg.key(fm.K_F5)
    lay = fm.wait_layout(dbg, win, lambda l: len(l.ops) >= 3 and l.ops.get(1, (0,))[0] == QUEUED
                         and l.ops.get(2, (0,))[0] == QUEUED, timeout=8.0) or fm.last_layout()
    res.check("two more copies wait behind it, each a waiting row",
              lay is not None and len(lay.ops) == 3 and lay.ops[1][0] == QUEUED and lay.ops[2][0] == QUEUED,
              f"rows={lay and lay.ops}")
    r = lay.opsact.get(2) if lay else None
    if r:
        click(dbg, qmp, lay, r)
    lay = fm.wait_layout(dbg, win, lambda l: len(l.ops) == 2, timeout=6.0) or fm.last_layout()
    res.check("Cancel on the last waiting row takes it out",
              lay is not None and len(lay.ops) == 2, f"rows={lay and lay.ops}")

    # 5.
    lay = fm.layout_now(dbg, win) or lay
    act = lay.opsact2.get(0) if lay else None
    if act and act[0] == EV_RESUME:
        click(dbg, qmp, lay, act[1])
    lay = fm.wait_layout(dbg, win, lambda l: 0 in l.ops and l.ops[0][0] == RUNNING and paused
                         and l.ops[0][1] > paused[1], timeout=10.0) or fm.last_layout()
    res.check("Resume goes on from where it stopped",
              lay is not None and 0 in lay.ops and paused is not None and lay.ops[0][1] > paused[1],
              f"resume={act} row={lay and lay.ops.get(0)} paused at {paused}")

    # 6.
    done = fm.wait_layout(dbg, win, lambda l: l.cancel == 0, timeout=120.0)
    res.check("everything ends, and the panel goes", done is not None,
              f"last rows={fm.last_layout() and fm.last_layout().ops}")
    sizes = [size_of(dbg, f"{ROOT}/dst/big/v{i}.mpg") for i in range(COPIES)]
    res.check("big/'s copies are whole on disk", sizes == [one] * COPIES, f"sizes={sizes}")
    small = (dbg.send(f"sh cat {ROOT}/dst/small.txt") or "").strip()
    res.check("small.txt was copied after it", small == "small but whole", repr(small))
    res.check("...and other.txt, cancelled while it waited, was not",
              size_of(dbg, f"{ROOT}/dst/other.txt") is None, "it is there")
    dbg.send(f"sh rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_progress_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_progress_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
