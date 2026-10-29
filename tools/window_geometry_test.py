#!/usr/bin/env python3
"""tools/window_geometry_test.py -- windows come back where you left them.

WHY THIS EXISTS
---------------
The compositor remembers each application's window position and size
across launches (`userland/wm/wm_geometry.c`, `/etc/windows.conf`). Two
halves can rot independently and neither is visible from a screenshot:
the SAVE, which happens once in close_window(), and the RESTORE, which
has to survive a screen that is no longer the size it was.

The restore half is the one with sharp edges, so most of this drives it
from a HAND-WRITTEN config rather than from a previous run. That is
deliberate: a round-trip test alone would pass if both halves were
broken in the same direction -- save the wrong thing, restore the wrong
thing, get it back. Writing a known geometry and requiring exactly that
geometry cannot be satisfied by a symmetric bug.

WHAT IT ASSERTS
---------------
- a restored window's FIRST frame is already the restored size -- not
  the app's default size followed by a resize, which shows as a flash
- a geometry that FITS is restored verbatim, including a window sitting
  over the taskbar (that is a legal placement here, so restoring must
  not "helpfully" move it -- an earlier version clamped to the work
  area and moved y=150 to y=120)
- a geometry far LARGER than the screen is clamped to the work area
- a NEGATIVE position is pulled back on screen, title bar reachable
- a real round trip: move a window, close it, reopen it, same geometry
- an app that has never been opened gets its own default, not another
  app's geometry
- THE KEY IS THE PROGRAM'S PATH (wm_app_key()): a save writes
  `/bin/wm/system/settings=`, a path-keyed line restores verbatim, and a
  line under the old declared-id key (`settings=`, which most checks
  above write) is still read, so nobody's sizes reset; two programs on
  ONE executable told apart by their sub-id (w16's hello and gdi, both
  /bin/w16) each restore their own
- a window that draws its own frame, saved at exactly the size it asks
  for, reopens at that size launch after launch -- not smaller by
  toywm's chrome each time
- THE STATE IT CLOSED IN: maximized writes `,max` after its own rect
  and reopens maximized (a client says it is resizable only in its
  hints, after the create that restored it -- the state waits for
  them), snapped writes `,snap:<zone>` and reopens in its half
- a drag's END is written at once, before any close
- `desktop.windows.remember off` neither writes nor restores, and on
  again the kept line comes back
- Forget Saved Position drops the line, and the close does not write it
  back

POSITIVE CONTROLS (the state checks, 2026-10-29): no `,max` suffix
reddens the five state checks; Forget not marking the window reddens its
two; no save at a drag's end reddens that check alone.

    python3 tools/vm.py start
    python3 tools/window_geometry_test.py
    echo $?
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard                                      # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
CONF = "/etc/windows.conf"
APP = "System Settings"
APP_ID = "settings"                      # the declared id: the legacy key
APP_KEY = "/bin/wm/system/settings"      # the path: the key a save writes


Result = Results


def close_all(dbg, timeout=12.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ws = dbg.json("gui windows --json")["windows"]
        if not ws:
            return True
        dbg.send(f"gui close {len(ws) - 1}")
        dbg.settle()
        time.sleep(0.4)
    return False


def open_and_read(dbg, app=APP, timeout=15.0):
    """Open `app` and return its window record once one exists."""
    dbg.open_app(app)
    deadline = time.time() + timeout
    while time.time() < deadline:
        dbg.settle()
        ws = dbg.json("gui windows --json")["windows"]
        if ws:
            return ws[-1]
        time.sleep(0.4)
    return None


def answered(dbg, want, timeout=10.0):
    """The newest window's geometry once its CLIENT has answered the
    restore -- or whatever it is at the deadline. The WM moves a window
    at once but a size is a REQUEST the client answers with a present
    (wm_geometry_restore()), so the record read the moment the window
    exists can still carry the default size: under a loaded host the
    Task Manager was read before it had drawn at the restored one."""
    deadline = time.time() + timeout
    g = None
    while time.time() < deadline:
        ws = dbg.json("gui windows --json")["windows"]
        if ws:
            w = ws[-1]
            g = (w["x"], w["y"], w["w"], w["h"])
            if g == want:
                return g
        dbg.settle()
        time.sleep(0.3)
    return g


def set_conf(dbg, line):
    dbg.send(f"sh write {CONF} {line}")
    time.sleep(0.5)


def run(dbg, qmp, res):
    screen = dbg.json("gui state --json")
    sw = screen.get("screen_w") or 1280
    sh = screen.get("screen_h") or 720
    tb = screen.get("taskbar_h") or 40
    print(f"  (screen {sw}x{sh}, taskbar {tb})")

    close_all(dbg)

    # 1. A geometry that fits comes back EXACTLY -- including a window
    #    whose bottom is under the taskbar, which is a placement the WM
    #    allows on purpose.
    want = (200, 150, 800, 560)
    set_conf(dbg, f"{APP_ID}={want[0]},{want[1]},{want[2]},{want[3]}")
    w = open_and_read(dbg)
    got = (w["x"], w["y"], w["w"], w["h"]) if w else None
    res.check("a geometry that fits is restored verbatim", got == want,
              f"wanted {want}, got {got}")
    # ...AND IT APPEARS AT THAT SIZE: its FIRST frame is already the
    # restored content size, not the app's default followed by a resize
    # -- the flash Wayland's initial configure exists to prevent.
    first = (w["first_frame"]["w"], w["first_frame"]["h"]) if w else None
    content = (w["content"]["w"], w["content"]["h"]) if w else None
    res.check("...and its first frame is already that size, with no flash of the default",
              bool(first) and first == content, f"first frame {first}, content {content}")
    close_all(dbg)

    # 2. Larger than the screen: clamped to the work area, not restored
    #    whole. A window wider than the screen has its close button off
    #    the right edge.
    set_conf(dbg, f"{APP_ID}=5000,5000,4000,3000")
    w = open_and_read(dbg)
    # EXACTLY the screen width, not merely "not too big". A default-sized
    # window is also "not too big", so `<=` would pass with the whole
    # feature disabled -- which is what the first version of this check
    # did, and the positive control caught it.
    ok = w and w["w"] == sw and w["h"] <= sh - tb
    res.check("an oversized saved geometry is clamped to the work area", bool(ok),
              f"got {w['w']}x{w['h']}, wanted width exactly {sw} and height <= {sh - tb}"
              if w else "no window")
    onscreen = w and 0 <= w["x"] <= sw - w["w"] and 0 <= w["y"] <= sh - w["h"]
    res.check("...and it lands fully on screen", bool(onscreen),
              f"at {w['x']},{w['y']}" if w else "no window")
    close_all(dbg)

    # 2b. AN APP WHOSE OWN DEFAULT IS TALLER THAN THE WORK AREA is fitted
    #     before its first frame, not shown at its default and shrunk a
    #     frame later: the Audio Player's 898x698 against a 1280x720
    #     screen. The fit is proposed during the create, like a restore,
    #     so uapp's drain answers it before drawing. Asserted only where
    #     the default really overflows -- elsewhere nothing is fitted.
    dbg.send(f"sh rm {CONF}")
    time.sleep(0.5)
    w = open_and_read(dbg, "Audio Player")
    # A window EXISTS before its first present, and until then its record
    # holds the create-time size and a first_frame of 0x0 -- so wait for
    # the frame this check is about, not merely the window.
    deadline = time.time() + 10.0
    while w and not w["first_frame"]["w"] and time.time() < deadline:
        time.sleep(0.3)
        dbg.settle()
        ws = dbg.json("gui windows --json")["windows"]
        w = ws[-1] if ws else None
    if w and w["h"] >= sh - tb:
        first = (w["first_frame"]["w"], w["first_frame"]["h"])
        content = (w["content"]["w"], w["content"]["h"])
        res.check("a default taller than the work area is fitted before its first frame",
                  w["h"] <= sh - tb and first == content,
                  f"window {w['w']}x{w['h']} (work area {sh - tb} high), first frame "
                  f"{first}, content {content}")
    else:
        print(f"  (skipped the oversized-default check: the Audio Player opened "
              f"{w['w']}x{w['h'] if w else '-'}, which fits this screen)" if w else
              "  (skipped the oversized-default check: no Audio Player window)")
    close_all(dbg)

    # 3. A negative position is pulled back, title bar reachable.
    set_conf(dbg, f"{APP_ID}=-400,-300,640,480")
    w = open_and_read(dbg)
    # The SIZE is asserted too, and that is what makes this check mean
    # something: 640x480 can only have come from the file, whereas a
    # non-negative position is true of the default placement as well.
    ok = w and w["x"] >= 0 and w["y"] >= 0 and (w["w"], w["h"]) == (640, 480)
    res.check("a negative saved position is pulled back on screen", bool(ok),
              f"at {w['x']},{w['y']} size {w['w']}x{w['h']}" if w else "no window")
    close_all(dbg)

    # 4. TWO APPS, TWO GEOMETRIES, each getting its own. A restore that
    #    ignored the key would hand both the same rect, and a restore
    #    that did nothing would hand both their defaults -- this fails
    #    on either.
    #
    #    BOTH RECTS MUST FIT THE SCREEN or the clamp fires and the check
    #    measures clamping instead of keying: 300,200,900,600 bottoms out
    #    at 800 on a 720-high screen, came back at y=120, and read as a
    #    failure of something else entirely.
    set_conf(dbg, f"{APP_ID}=300,100,900,600")  # fits 1280x720; see below
    # Above Task Manager's own MINIMUM: an app clamps a proposal below it
    # (uapp.c's WIN_EV_RESIZE), and at 520 wide this measured the clamp.
    dbg.send("sh append /etc/windows.conf taskmgr=40,60,960,480")
    time.sleep(0.5)
    a = open_and_read(dbg)
    a_geom = answered(dbg, (300, 100, 900, 600)) if a else None
    close_all(dbg)
    b = open_and_read(dbg, "Task Manager")
    b_geom = answered(dbg, (40, 60, 960, 480)) if b else None
    res.check("each app restores its OWN geometry",
              a_geom == (300, 100, 900, 600) and b_geom == (40, 60, 960, 480),
              f"System Settings {a_geom}, Task Manager {b_geom}")
    close_all(dbg)

    # 5. THE SAVE HALF, exercised on its own. Everything above drives
    #    RESTORE from a written file and would still pass if
    #    close_window() never recorded anything. Here the window is
    #    actually DRAGGED and the file is read back, so the two halves
    #    are covered independently rather than only as a round trip --
    #    which a symmetric bug would satisfy.
    dbg.send(f"sh rm {CONF}")
    time.sleep(0.5)
    w = open_and_read(dbg)
    if not w:
        res.check("a drag is recorded on close", False, "no window opened")
        return
    tb_h = w["content"]["y"] - w["y"]
    grab_x = w["x"] + w["w"] // 2
    grab_y = w["y"] + max(1, tb_h // 2)
    qmp.drag(grab_x, grab_y, grab_x + 90, grab_y + 70)
    dbg.settle()
    time.sleep(0.6)
    moved = dbg.json("gui windows --json")["windows"]
    moved_geom = (moved[-1]["x"], moved[-1]["y"]) if moved else None
    close_all(dbg)
    time.sleep(0.6)
    saved = (dbg.send(f"sh cat {CONF}") or "")
    line = [ln for ln in saved.splitlines() if ln.startswith(APP_KEY + "=")]
    ok = False
    if line and moved_geom:
        parts = line[0].split("=", 1)[1].split(",")
        ok = len(parts) == 4 and (int(parts[0]), int(parts[1])) == moved_geom
    res.check("a dragged window's new position is recorded on close, keyed by its path", ok,
              f"window ended at {moved_geom}, file says {saved.splitlines()}")
    close_all(dbg)

    # 5b. A PATH-KEYED LINE restores verbatim -- the key a save writes.
    set_conf(dbg, f"{APP_KEY}=260,140,820,520")
    w = open_and_read(dbg)
    got = (w["x"], w["y"], w["w"], w["h"]) if w else None
    res.check("a geometry saved under the program's path is restored", got == (260, 140, 820, 520),
              f"got {got}")
    close_all(dbg)

    # 5c. ONE EXECUTABLE, TWO PROGRAMS: w16 runs both samples, and each
    #     restores its own size, keyed path:sub-id. One shared key gave
    #     every Win16 program the last one's size.
    set_conf(dbg, "/bin/w16:w16-hello=100,100,420,260")
    dbg.send("sh append /etc/windows.conf /bin/w16:w16-gditest=520,120,300,220")
    time.sleep(0.5)
    hello = dbg.spawn("/bin/w16 /usr/share/w16/hello.exe", "Hello (Win16)")
    hg = (hello["x"], hello["y"], hello["w"], hello["h"]) if hello else None
    dbg.close_window("Hello (Win16)")
    gdi = dbg.spawn("/bin/w16 /usr/share/w16/gdi.exe", "GDI test (Win16)")
    gg = (gdi["x"], gdi["y"], gdi["w"], gdi["h"]) if gdi else None
    dbg.close_window("GDI test (Win16)")
    res.check("two programs on one executable each restore their own geometry",
              hg == (100, 100, 420, 260) and gg == (520, 120, 300, 220),
              f"hello {hg}, gdi {gg}")
    close_all(dbg)

    # 5d. A CLIENT-FRAMED WINDOW SAVED AT THE SIZE IT ASKS FOR keeps it,
    #     launch after launch. It is restored twice before its first
    #     frame (with toywm's chrome subtracted, then without), and when
    #     the second answer equalled its own size it was never sent: the
    #     window lost the chrome at every launch -- w16's Control Panel,
    #     which saves its size and asks for it again.
    dbg.send(f"sh write {CONF} ")
    time.sleep(0.5)
    first = dbg.spawn("/bin/w16 /usr/share/w16/hello.exe", "Hello (Win16)")
    own = (first["w"], first["h"]) if first else None
    dbg.close_window("Hello (Win16)")
    sizes = []
    for _ in range(2):
        if own:
            set_conf(dbg, f"/bin/w16:w16-hello=120,110,{own[0]},{own[1]}")
            time.sleep(0.5)
        again = dbg.spawn("/bin/w16 /usr/share/w16/hello.exe", "Hello (Win16)")
        sizes.append((again["w"], again["h"]) if again else None)
        dbg.close_window("Hello (Win16)")
    res.check("a client-framed window saved at its own size reopens at it, twice", own is not None
              and sizes == [own, own], f"its own {own}, reopened at {sizes}")
    close_all(dbg)

    # 6. A FIXED-SIZE APP KEEPS ITS OWN SIZE, whatever the file says.
    #    Geometry is restored as a PROPOSAL and a client that never
    #    declared itself resizable declines it -- without that, one
    #    wrong size is permanent, since it is written back on every
    #    close. The position is still honoured: only the size is the
    #    app's to refuse.
    #
    #    Calculator is the fixed-size app; the saved size below is
    #    far larger than its natural one, so a window that accepted it
    #    is obvious rather than marginal -- but still small enough not
    #    to trip the on-screen clamp, which check 2 covers and which
    #    would move the POSITION as well and confuse this check.
    dbg.send(f"sh write {CONF} calculator=180,120,700,500")
    time.sleep(0.5)
    w = open_and_read(dbg, app="Calculator")
    pos = (w["x"], w["y"]) if w else None
    size = (w["w"], w["h"]) if w else None
    res.check("a fixed-size app refuses a saved size", bool(w) and size != (700, 500),
              f"opened at {size}, the file asked for (700, 500)")
    res.check("...and still takes the saved position", pos == (180, 120),
              f"opened at {pos}")
    close_all(dbg)

    placement_states(dbg, qmp, res, sw, sh, tb)


KEY_UP, KEY_DOWN, KEY_LEFT = 0xF780, 0xF781, 0xF784


def conf_line(dbg, key=APP_KEY):
    for ln in (dbg.send(f"sh cat {CONF}") or "").splitlines():
        if ln.strip().startswith(key + "="):
            return ln.strip()[len(key) + 1:]
    return None


def newest(dbg):
    ws = dbg.json("gui windows --json")["windows"]
    return ws[-1] if ws else {}


def placement_states(dbg, qmp, res, sw, sh, tb):
    """7. The STATE a window closed in, a save at a drag's END rather than
    at close, the Settings switch, and Forget Saved Position.

    Each reads /etc/windows.conf itself, so the SAVE is asserted apart
    from the restore: a restore that ignored the state field would pass
    any "it reopened" check whose window happened to open maximized."""
    from harness import poll
    dbg.send(f"sh rm {CONF}")
    w = open_and_read(dbg)
    if not w:
        res.check("System Settings opens for the state checks", False)
        return
    own = (w["w"], w["h"])
    dbg.key(hex(KEY_UP), mods="super")
    line = poll(lambda: (lambda v: v if v and v.endswith(",max") else None)(conf_line(dbg)), 5)
    res.check("maximized, the window's line says max -- with its own size, not the screen's",
              bool(line) and line.split(",")[2:4] == [str(own[0]), str(own[1])],
              f"line {conf_line(dbg)!r}, its own size {own}")
    close_all(dbg)
    w = open_and_read(dbg)
    st = poll(lambda: (lambda v: v if v.get("state") == "maximized" else None)(newest(dbg)), 5) or newest(dbg)
    res.check("...and it reopens maximized", st.get("state") == "maximized", f"state {st.get('state')}")
    dbg.key(hex(KEY_DOWN), mods="super")
    back = poll(lambda: (lambda v: v if v.get("state") == "normal" and (v["w"], v["h"]) == own else None)(newest(dbg)), 5) \
        or newest(dbg)
    res.check("...and restores to its own size", back.get("state") == "normal" and (back.get("w"), back.get("h")) == own,
              f"{back.get('state')} at {back.get('w')}x{back.get('h')}, its own {own}")

    # Snapped: the zone rides after the rect.
    dbg.key(hex(KEY_LEFT), mods="super")
    dbg.key("0x1b")          # Snap Assist for the other half: not now
    line = poll(lambda: (lambda v: v if v and ",snap:" in v else None)(conf_line(dbg)), 5)
    res.check("snapped, the line carries the zone", bool(line), f"line {conf_line(dbg)!r}")
    close_all(dbg)
    open_and_read(dbg)
    st = poll(lambda: (lambda v: v if v.get("snap") == "left" else None)(newest(dbg)), 5) or newest(dbg)
    res.check("...and it reopens in the left half", st.get("snap") == "left" and st.get("x") == 0,
              f"snap {st.get('snap')!r} at {st.get('x')},{st.get('y')}")

    # A drag's end is saved at once, before any close -- on a PLAIN window,
    # so no snap or maximize along the way writes the line for it.
    close_all(dbg)
    dbg.send(f"sh rm {CONF}")
    w = open_and_read(dbg)
    band = (w["content"]["y"] - w["y"]) // 2
    start = (w["x"], w["y"])
    dbg.drag(w["x"] + 60, w["y"] + band, w["x"] + 60 + 90, w["y"] + band + 70, 16)
    moved = poll(lambda: (lambda v: v if (v.get("x"), v.get("y")) != start else None)(newest(dbg)), 5) \
        or newest(dbg)
    line = poll(lambda: (lambda v: v if v and v.split(",")[:2] == [str(moved.get("x")), str(moved.get("y"))]
                         else None)(conf_line(dbg)), 5)
    res.check("a drag's end is written at once, the window still open",
              bool(line) and (moved.get("x"), moved.get("y")) != start,
              f"window {start} -> {moved.get('x')},{moved.get('y')}, line {conf_line(dbg)!r}")

    # The Settings switch: off, nothing is read or written.
    dbg.send("sh config set desktop.windows.remember off")
    before = conf_line(dbg)
    w = newest(dbg)
    dbg.drag(w["x"] + 60, w["y"] + band, 300, 220, 16)
    dbg.settle()
    res.check("with remembering off, a drag writes nothing", conf_line(dbg) == before,
              f"{before!r} -> {conf_line(dbg)!r}")
    close_all(dbg)
    dbg.send(f"sh write {CONF} {APP_KEY}=210,130,760,500")
    w = open_and_read(dbg)
    got = answered(dbg, (210, 130, 760, 500), timeout=3) if w else None
    res.check("...and a saved geometry is not restored", got != (210, 130, 760, 500), f"opened at {got}")
    close_all(dbg)
    dbg.send("sh config unset desktop.windows.remember")
    w = open_and_read(dbg)
    got = answered(dbg, (210, 130, 760, 500)) if w else None
    res.check("switched back on, the kept line is restored again", got == (210, 130, 760, 500), f"opened at {got}")

    # Forget Saved Position, from the window menu (Alt+Space).
    dbg.key("0x20", mods="alt")     # a bare " " is lost to the command's own spacing
    row = poll(lambda: dbg.ctxmenu_row("Forget Saved Position"), 3)
    if row:
        dbg.click(*row)
        dbg.settle()
    gone = poll(lambda: conf_line(dbg) is None, 5)
    close_all(dbg)
    res.check("Forget Saved Position drops the line, and closing does not write it back",
              bool(row) and bool(gone) and conf_line(dbg) is None, f"row {row}, line {conf_line(dbg)!r}")
    w = open_and_read(dbg)
    got = (w["x"], w["y"], w["w"], w["h"]) if w else None
    res.check("...so the next window opens where a new one would", got and got[:2] != (210, 130),
              f"opened at {got}")
    close_all(dbg)


def main():
    ap = argparse.ArgumentParser()
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "window_geometry_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()

    print(f"\nwindow_geometry_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
