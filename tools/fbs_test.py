#!/usr/bin/env python3
"""First-boot setup: shown when it should be, saves what is chosen, ends
with the marker, and its full screen keeps the taskbar out.

WHAT IT CHECKS, and what a broken version would still pass:

  - `firstboot --boot` on a MARKED machine opens nothing and exits, and
    after `fbs again` the same command opens the wizard -- the boot
    service's whole decision, without a reboot. A wizard that always
    opened passes the second half only;
  - the window is FULLSCREEN, covering the screen, and a click where the
    tray sits opens no flyout -- the closed-overlay fix in wm_overlay.c
    (the network flyout used to open over a fullscreen app);
  - a layout chosen with the ARROW KEY is saved at once: `config get` (an
    independent reader) shows the new system.keyboard.layout -- a wizard
    that only stored the choice for later passes everything else here;
  - an invalid computer name ("bad name") keeps Next on its step, and a
    valid one moves on and lands in system.hostname;
  - Next on the last step ends it: the window goes and `fbs` reports
    "finished"; Skip, then Return on the question, ends it "skipped";
  - the wizard STARTS at the recommended display scale: 1280 wide leaves
    100%, and at 1920x1080 (QEMU's EDID says 32 cm across) opening it
    applies 125% at once, which Skip keeps.

Geometry is the client's own report (`gui widgets`), never pixels here.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results, poll  # noqa: E402

WIZARD = "/bin/wm/system/firstboot"
TITLE = "First-boot setup"
K_DOWN, K_ENTER, K_BS = "0xf781", "0x0a", "0x08"   # api/keyboard.h
HEX = {" ": "0x20", "-": "0x2d"}

_res = Results()
check = _res.check


def sh(dbg, cmd):
    return dbg.send(f"sh {cmd}")


def setting(dbg, name):
    out = sh(dbg, f"config get {name}")
    lines = [ln.strip() for ln in out.splitlines() if ln.strip() and not ln.startswith("--")]
    return lines[-1] if lines else ""


def window(dbg):
    return next((w for w in dbg.windows() if w["title"] == TITLE), None)


def wait_window(dbg, present, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        if (window(dbg) is not None) == present:
            return True
        time.sleep(0.3)
    return False


def wait_ready(dbg, timeout=10.0):
    """Listed is not shown: the WM has the window before its fullscreen
    proposal lands and before the client has drawn (and reported) a
    control. Waits for both, bounded; the window dict, or None."""
    end = time.time() + timeout
    while time.time() < end:
        w = window(dbg)
        if w and w["state"] == "fullscreen" and "next" in dbg.widgets(TITLE):
            return w
        time.sleep(0.3)
    return window(dbg)


def step_seen(dbg, n, timeout=6.0):
    """The wizard logged `firstboot: step n` since the last read."""
    end = time.time() + timeout
    while time.time() < end:
        if any(f"firstboot: step {n}" in ln for ln in dbg.logs("firstboot:", clear=False)):
            return True
        time.sleep(0.3)
    return False


def click_widget(dbg, name):
    x, y = dbg.widget_center(name, TITLE)
    dbg.click(x, y)


def type_text(dbg, text):
    for ch in text:
        dbg.send(f"gui key {HEX.get(ch, ch)}")
    dbg.settle()


def finish():
    return _res.finish("fbs_test")


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fbs_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("First-boot setup (lib/ufbs.h, ui/uui_wizard.h)")

    st = sh(dbg, "fbs")
    check("a test image is pre-marked: fbs says done", "done" in st, st.strip()[-160:])
    dbg.send(f"gui spawn {WIZARD} --boot")
    time.sleep(3)
    check("--boot on a marked machine opens no window", window(dbg) is None)

    st = sh(dbg, "fbs again") + sh(dbg, "fbs")
    check("fbs again removes the marker", "not done" in st, st.strip()[-200:])
    dbg.logs("firstboot:", clear=True)
    dbg.send(f"gui spawn {WIZARD} --boot")
    if not check("...and then --boot opens the wizard", wait_window(dbg, True)):
        return finish()
    w = wait_ready(dbg)
    state = dbg.state()
    sw, sh_, tb = state["screen"]["w"], state["screen"]["h"], state["taskbar_h"]
    check("it is fullscreen and covers the screen",
          w["state"] == "fullscreen" and w["x"] == 0 and w["y"] == 0 and w["w"] >= sw and w["h"] >= sh_,
          f"{w['state']} {w['x']},{w['y']} {w['w']}x{w['h']} on {sw}x{sh_}")
    check(f"{sw} wide recommends 100%, so the scale is left there",
          setting(dbg, "display.scale") == "100", setting(dbg, "display.scale"))

    # Across the middle of the taskbar's band, where the tray icons sit,
    # stepping round the wizard's own buttons by their reported rects.
    ws = dbg.widgets(TITLE)
    mine = [(ws[n]["screen"]["x"], ws[n]["screen"]["y"], ws[n]["w"], ws[n]["h"])
            for n in ("back", "next") if n in ws]
    band_y = sh_ - tb // 2
    for x in range(sw // 2, sw - 4, 16):
        if any(bx - 4 <= x < bx + bw + 4 and by - 4 <= band_y < by + bh + 4 for bx, by, bw, bh in mine):
            continue
        dbg.click(x, band_y, settle=False)
    dbg.settle()
    opened = [k for k, v in dbg.state()["overlays"].items() if v]
    check("a click where the tray sits opens no flyout over it", not opened, f"{opened}")

    # Keyboard: the arrow key's choice is saved.
    click_widget(dbg, "next")
    check("Next moves to the Keyboard step", step_seen(dbg, 1))
    before = setting(dbg, "system.keyboard.layout")
    dbg.key(K_DOWN)
    time.sleep(1)
    after = setting(dbg, "system.keyboard.layout")
    check("a layout chosen with the arrow key is saved (config get)", after and after != before,
          f"{before!r} -> {after!r}")

    for n in (2, 3, 4):
        click_widget(dbg, "next")
        step_seen(dbg, n)
    check("Next reaches the Computer name step", step_seen(dbg, 4))

    click_widget(dbg, "name")
    for _ in range(12):
        dbg.send(f"gui key {K_BS}")
    type_text(dbg, "bad name")
    dbg.logs("firstboot:", clear=True)
    click_widget(dbg, "next")
    check("an invalid name keeps Next on its step", not step_seen(dbg, 5, timeout=3))
    for _ in range(12):
        dbg.send(f"gui key {K_BS}")
    type_text(dbg, "fbs-test")
    click_widget(dbg, "next")
    check("a valid name moves on", step_seen(dbg, 5))
    check("...and is saved as system.hostname", setting(dbg, "system.hostname") == "fbs-test",
          setting(dbg, "system.hostname"))

    click_widget(dbg, "next")
    check("Next reaches Ready", step_seen(dbg, 6))
    click_widget(dbg, "next")
    check("the last Next closes the wizard", wait_window(dbg, False))
    st = sh(dbg, "fbs")
    check("...and fbs reports it finished", "finished" in st, st.strip()[-160:])

    # Skip, from a wizard run by hand.
    sh(dbg, "fbs now")
    if check("fbs now opens it on a set-up machine", wait_window(dbg, True)):
        wait_ready(dbg)
        click_widget(dbg, "skip")
        dbg.key(K_ENTER)
        check("Skip, then Return on the question, closes it", wait_window(dbg, False))
        st = sh(dbg, "fbs")
        check("...and fbs reports it skipped", "skipped" in st, st.strip()[-160:])

    # The recommended scale is where the wizard starts (firstboot.c's
    # preselect_scale()), on a wider screen than the run above.
    orig = (sw, sh_)
    sh(dbg, "config set resolution 1920x1080")
    dbg.settle()
    try:
        sh(dbg, "fbs again")
        dbg.send(f"gui spawn {WIZARD} --boot")
        if check("at 1920x1080 the wizard opens again", wait_window(dbg, True)):
            wait_ready(dbg)
            got = poll(lambda: setting(dbg, "display.scale") == "125" or None, 15)
            check("...and starts at the recommended 125%, applied", got,
                  setting(dbg, "display.scale"))
            click_widget(dbg, "skip")
            dbg.key(K_ENTER)
            check("Skip closes it and keeps 125%",
                  wait_window(dbg, False) and setting(dbg, "display.scale") == "125",
                  setting(dbg, "display.scale"))
    finally:
        sh(dbg, "config set display.scale 100")
        sh(dbg, f"config set resolution {orig[0]}x{orig[1]}")
        dbg.settle()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
