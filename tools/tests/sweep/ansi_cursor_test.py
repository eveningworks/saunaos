#!/usr/bin/env python3
"""ANSI cursor movement, erasing and the scrolling region, checked as
PIXELS -- on the physical console, and in a GUI Terminal window.

kernel/lib/ansi.c is a pure state machine and its KTESTs assert what a
sequence RESOLVES to with no display at all. This is the other half:
whether kernel/drivers/vga.c then puts ink in the right cell. Only
pixels can answer that, which is why this is a Python tool and not a
KTEST.

THE DESKTOP OWNS THE SCREEN, so it has to go first. This deletes the
toywm service descriptor and kills the process -- the same
`unsupervise then kill` pattern compositor_death_test.py uses, and for
the same reason: init restarts a killed desktop with a zero backoff, so
killing it without removing the descriptor races a replacement that has
already re-claimed the framebuffer. The image is a throwaway copy, so
deleting a seeded file costs nothing.

EVERY CHECK HAS A BLANK NEIGHBOUR. "Something was drawn" is satisfied by
a console that ignores cursor movement and prints everything in a row;
only the gaps tell that apart from working.

THE SCROLLING REGION IS READ AS NUMBERED ROWS. /tests/ansiregion marks
row n at column 20 + n, then moves the rows about inside a DECSTBM region
(IL, DL, SU, SD, RI, line feeds on the bottom margin;
userland/tests/ansiregion_seq.h). Which column holds a row's mark says
WHICH row it is, so a shift by the wrong amount, or one that leaked out
of the region, reads as the wrong number rather than as "ink". It runs in
BOTH terminals -- the console's renderer is vga.c, the window's is
uvterm.c, and the rules they share are ansi.c's -- and then writes `#\n#`
twice, with OPOST on and off: column 1 of the next row, then column 2
(the tty's ONLCR, then a bare line feed). Run on demand:

    python3 tools/tests/sweep/ansi_cursor_test.py [--instance N]
"""

RUNNER = {"name": "ansi", "covers": "ANSI cursor movement, as pixels"}

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from qmp_test import QMPSession                          # noqa: E402
from gui_debug import DebugConsole                       # noqa: E402
import port_guard                                        # noqa: E402
import harness                                           # noqa: E402

HERE = toolpath.TOOLS
VM = toolpath.tool("vm.py")


INSTANCE = 0   # set from --instance in main()


def vm(*argv, check=False):
    """One shell command in the guest, through tools/lib/vm.py.

    Built on vm.py rather than on launch_qemu_cmd() directly because
    vm.py already owns the pieces this needs -- a booted guest, a serial
    socket to talk to it on, and a QMP port to photograph it through --
    and rederiving that here would be a second copy of the launch dance
    the module docstring of qmp_test.py exists to prevent.
    """
    r = subprocess.run([sys.executable, VM, "--instance", str(INSTANCE)] + list(argv),
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        print(r.stdout + r.stderr)
    return r.stdout + r.stderr


TOYWM_SVC = "/etc/services.d/toywm"
CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"    [{detail}]" if detail else ""))


def ink_bands(values):
    """Contiguous runs of True in `values`, as (start, end) pairs."""
    out = []
    for i, v in enumerate(values):
        if not v:
            continue
        if out and i == out[-1][1] + 1:
            out[-1][1] = i
        else:
            out.append([i, i])
    return out


def calibrate(img, fallback):
    """Derive the cell size from the pattern rather than assuming it.

    Rows 3 and 6 of the pattern both carry ink and are three cells
    apart; the two marks on row 3 are four columns apart. Both come out
    of the same image the checks then read, so a font change cannot put
    the two out of step.

    Falls back to `fallback` (and says so) if the pattern is not there
    at all -- in which case every check is about to fail anyway, and
    failing on the checks is more informative than failing here.
    """
    w, h = img.size
    bg = img.getpixel((0, 0))
    rows = ink_bands([any(img.getpixel((x, y)) != bg for x in range(0, min(w, 240)))
                      for y in range(h)])
    cols = ink_bands([any(img.getpixel((x, y)) != bg for y in range(0, min(h, 240)))
                      for x in range(min(w, 240))])
    if len(rows) < 2 or len(cols) < 2:
        return fallback
    ch = (rows[1][0] - rows[0][0]) // 3
    # The second column band is the row-3 mark at column 9; the first
    # band starts at column 1. Eight columns between their left edges.
    cw = (cols[1][0] - cols[0][0]) // 8
    if ch < 4 or cw < 4:
        return fallback
    return cw, ch


def cell_has_ink(img, cw, ch, row, col, bg, origin=(0, 0)):
    """True if any pixel in the 1-BASED cell (row, col) differs from `bg`.
    `origin` is the grid's top-left pixel: (0, 0) on the console.

    A margin is trimmed off every edge: a glyph does not fill its cell,
    and neighbouring cells' antialiasing (and the cursor block's border,
    when one is showing) can bleed a pixel. Sampling the middle asks
    about THIS cell rather than about the seam.
    """
    x0 = origin[0] + (col - 1) * cw
    y0 = origin[1] + (row - 1) * ch
    mx = max(1, cw // 6)
    my = max(1, ch // 6)
    for y in range(y0 + my, y0 + ch - my):
        for x in range(x0 + mx, x0 + cw - mx):
            if img.getpixel((x, y)) != bg:
                return True
    return False


# userland/tests/ansiregion_seq.h's AR_WANT and AR_COL0 -- what row 1..14
# holds after the steps, by number (0 = blank).
ANSIREGION_WANT = [1, 2, 0, 7, 8, 9, 10, 0, 0, 0, 11, 12, 13, 14]
AR_COL0 = 20


def check_region(label, ink):
    """The numbered rows, and the line-feed pair, read through `ink(r, c)`."""
    got = []
    for n in range(1, len(ANSIREGION_WANT) + 1):
        marks = [k for k in range(1, len(ANSIREGION_WANT) + 1) if ink(n, AR_COL0 + k)]
        got.append(marks[0] if len(marks) == 1 else (0 if not marks else marks))
    check(f"{label}: the region's steps left rows 1-14 numbered as expected",
          got == ANSIREGION_WANT, f"want {ANSIREGION_WANT} got {got}")
    check(f"{label}: OPOST on -- `#\\n#` starts the next row at column 1",
          ink(16, 1) and ink(17, 1) and not ink(17, 2))
    check(f"{label}: OPOST off -- a bare line feed keeps the column (row 20, column 2)",
          ink(19, 1) and ink(20, 2) and not ink(20, 1))


def terminal_phase(dbg, qmp, tmp):
    """/tests/ansiregion in a GUI Terminal window, its grid read as cells.

    The geometry comes from the Terminal's own layout line (cell size,
    chrome height, margin), not from a guess: the grid's font is a
    setting."""
    dbg.send("sh config set desktop.layout_log on")
    win = dbg.spawn("/bin/wm/apps/uterm", title="Terminal")
    check("a Terminal window opened", win is not None)
    if not win:
        return
    dbg.settle()
    win = dbg.window("Terminal")
    c = win["content"]
    dbg.click(c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
    dbg.settle()
    hexkeys = {"/": "0x2f", " ": "0x20"}
    for ch in "/tests/ansiregion":
        dbg.send(f"gui key {hexkeys.get(ch, ch)}")
    dbg.settle()
    def field(name, n=1):
        for line in reversed(dbg.logs("uterm: layout cursor", clear=False)):
            parts = line.split()
            if name in parts:
                i = parts.index(name)
                return [int(x) for x in parts[i + 1:i + 1 + n]]
        return None

    dbg.send("gui key 0x0d")
    # DONE PAINTING once the cursor has reached row 22, where ansiregion
    # moves it after its last mark (its stderr line, which lands in this
    # window too, can take it a row further), as the window's own layout
    # line says.
    def painted():
        dbg.settle()
        cur, cols = field("cursor"), field("cols")
        return cur and cols and cur[0] >= 21 * cols[0]
    check("ansiregion ran in the Terminal (its cursor reached row 22)",
          harness.poll(painted, 15.0), f"cursor {field('cursor')} cols {field('cols')}")
    chrome, margin, cell = field("chrome"), field("margin"), field("cell", 2)
    check("the Terminal reports its grid geometry", chrome and margin and cell,
          f"chrome {chrome} margin {margin} cell {cell}")
    if not (chrome and margin and cell):
        return
    shot = os.path.join(tmp, "ansiregion_terminal.png")
    qmp.screenshot(shot)
    from PIL import Image
    img = Image.open(shot).convert("RGB")
    cw, ch = cell
    origin = (c["x"] + margin[0], c["y"] + chrome[0] + margin[0])
    bg = img.getpixel((origin[0] + cw // 2, origin[1] + ch // 2))   # row 1 col 1: blank
    check_region("Terminal", lambda r, k: cell_has_ink(img, cw, ch, r, k, bg, origin))
    dbg.close_window("Terminal")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="leave the VM running")
    ap.add_argument("--instance", type=int, default=0, metavar="N",
                    help="the VM slot to boot and drive (vm.py's numbering)")
    # The console's cell size, which vga.c derives from gfx_char_w/h.
    # Passed rather than probed because there is no shell command that
    # reports it, and a wrong guess fails LOUDLY here (every cell reads
    # blank) rather than quietly.
    # Only a FALLBACK now: the cell size is derived from the pattern
    # (see calibrate()), because this project's layout is font-derived
    # and a hardcoded size is exactly the kind of constant that rots.
    ap.add_argument("--cell", nargs=2, type=int, default=(8, 14),
                    metavar=("W", "H"))
    args = ap.parse_args()
    global INSTANCE
    INSTANCE = args.instance
    tmp = os.path.join("/tmp", f"ansi_cursor.{os.getpid()}")
    os.makedirs(tmp, exist_ok=True)

    try:
        from PIL import Image
    except ImportError:
        print("ansi_cursor_test: needs Pillow (pip install pillow)")
        return 2

    vm("stop")
    print("booting")
    vm("start", check=True)
    try:
        # THE WINDOW FIRST, while the desktop is up; the console after it
        # has been taken away.
        session = QMPSession(port=port_guard.instance_qmp(args.instance))
        dbg = DebugConsole(port_guard.instance_sock(args.instance))
        try:
            terminal_phase(dbg, session, tmp)
        finally:
            dbg.close()   # vm.py's `exec` below talks on the same socket

        # Take the desktop out of init's hands, then kill it, so the
        # physical console is what the framebuffer shows.
        # Copied aside and put back in the finally: the image is shared
        # with every later tool in a sweep (2026-09-30).
        vm("exec", f"cp {TOYWM_SVC} /var/tmp/toywm.service.saved")
        vm("exec", f"rm {TOYWM_SVC}")
        ps = vm("exec", "ps")
        pid = None
        for line in ps.splitlines():
            if "toywm" in line:
                pid = line.split()[0]
                break
        check("found the desktop to stop", pid is not None, ps.strip()[:60])
        if pid:
            vm("exec", f"kill {pid}")
            time.sleep(1.5)

        vm("exec", "spawn /tests/ansidraw")
        time.sleep(2.0)

        shot = os.path.join(tmp, "ansi_cursor.png")
        session.screenshot(shot)
        img = Image.open(shot).convert("RGB")

        # SELF-CALIBRATING, and it has to be. The console's cell size is
        # FONT-DERIVED (gfx_char_w/h), which this project treats as a
        # one-line change -- so a hardcoded 8x16 here would silently
        # start reading the wrong cells the day the default font size
        # moves. It did: the first version assumed 16 and the real
        # height is 14, which showed up as every row after the third
        # being "one row high" while the kernel was correct.
        #
        # The PATTERN carries the calibration. The marks are three rows
        # apart by construction, so the gap between the first two ink
        # bands is three cells; the two marks on the first row are four
        # columns apart.
        cw, ch = calibrate(img, args.cell)
        print(f"  note   calibrated to {cw}x{ch} cells from the pattern itself")
        # The background is read from a corner AFTER the clear rather
        # than assumed, so a theme change cannot silently make every
        # check pass by making everything look blank.
        bg = img.getpixel((cw // 2, ch // 2))

        def ink(r, c):
            return cell_has_ink(img, cw, ch, r, c, bg)

        # --- absolute positioning, with a gap ---
        check("a mark landed at row 3 col 5", ink(3, 5))
        check("and another at col 9", ink(3, 9))
        check("with the cells BETWEEN them blank",
              not ink(3, 6) and not ink(3, 7) and not ink(3, 8))

        # --- erase to end of line ---
        check("row 6 columns 1-4 survived the erase",
              ink(6, 1) and ink(6, 2) and ink(6, 3) and ink(6, 4))
        check("and columns 5-8 were erased",
              not ink(6, 5) and not ink(6, 6) and not ink(6, 7) and not ink(6, 8))

        # --- relative movement ---
        check("a relative move landed on row 9 col 5", ink(9, 5))
        check("with columns 1-4 blank",
              not ink(9, 1) and not ink(9, 2) and not ink(9, 3) and not ink(9, 4))

        # --- an explicit zero is a default ---
        check("ESC[0A moved up exactly one row (row 11, not 12)",
              ink(11, 3) and not ink(12, 3))

        # --- the clear actually cleared ---
        check("the screen above the pattern is blank",
              not ink(1, 1) and not ink(2, 1) and not ink(1, 40))

        # --- the scrolling region and the line feed, on the console ---
        vm("exec", "spawn /tests/ansiregion")
        # Its "painted" line reaches the kernel log after the last write,
        # whose cursor move has presented the screen.
        check("ansiregion ran on the console",
              harness.poll(lambda: "ansiregion: painted" in vm("exec", "dmesg"), 15.0))
        shot = os.path.join(tmp, "ansiregion_console.png")
        session.screenshot(shot)
        img = Image.open(shot).convert("RGB")
        check_region("console", lambda r, k: cell_has_ink(img, cw, ch, r, k, bg))
    finally:
        vm("exec", f"cp /var/tmp/toywm.service.saved {TOYWM_SVC}")
        if not args.keep:
            vm("stop")

    passed = sum(1 for _, ok, _ in CHECKS if ok)
    print(f"\nansi_cursor_test: {passed}/{len(CHECKS)} checks passed")
    return 0 if passed == len(CHECKS) else 1


if __name__ == "__main__":
    sys.exit(main())
