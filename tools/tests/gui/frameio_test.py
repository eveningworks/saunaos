#!/usr/bin/env python3
"""The desktop's thread reads no config file, decodes no icon and no
wallpaper when something changes: the worker does, the frame swaps.

WHAT IS UNDER TEST
------------------
toywm's config pollers re-read their settings whenever /etc or a setting
changes, an app-list reload used to drop the icon cache, and a new
wallpaper was decoded inside the frame -- each read able to wait out the
disk and the root volume's lock (a 2.15 s frame on the ASUS). Now a
change is published to the pollers only after the worker has re-read the
cached config files (wm_watch.c), an app list arrives with its icons
decoded (wm_apps_worker.c), and the wallpaper decodes on the worker while
the old one stays up (desktop.c).

Counted, not timed: `gui frameio` reports the config files the WM's OWN
thread read from disk and the icons icon_get() decoded itself. Each check
first proves the change reached the code (a refresh ran and read files, a
new entry loaded, the new wallpaper is the one shown), so "nothing was read
on the frame" cannot pass because nothing happened.

Usage (the VM must already be up):

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/frameio_test.py
    python3 tools/lib/vm.py stop
"""

RUNNER = {"name": "frameio", "covers": "config refresh, icon and wallpaper decodes off the WM's thread"}

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ENTRY = "/usr/wm/applications/zzframeio.desktop"
PROBE_CONF = "/etc/frameio_probe.conf"

_res = Results()
check = _res.check


def frameio(dbg):
    return dbg.json("gui frameio")


def wait_refresh(dbg, before, timeout=20.0):
    """Until a refresh newer than `before` has been PUBLISHED and the
    worker is idle -- settle() alone says the frame is drawn, not that a
    publish happened."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        dbg.settle()
        f = frameio(dbg)
        if f["refreshes"] > before["refreshes"] and f["pending"] == 0:
            return f
        time.sleep(0.2)   # sleep-ok: a guest-side counter, polled
    return frameio(dbg)


def settings_check(dbg):
    # A warm-up change first: a poller asking for a file the cache has
    # never held reads it once, which is the cold start, not a refresh.
    dbg.send(f"sh write {PROBE_CONF} warm=1")
    wait_refresh(dbg, frameio(dbg))
    dbg.send("sh config set desktop.wallpaper.mode fit")
    wait_refresh(dbg, frameio(dbg))

    # 1. An unrelated /etc write: every config poller re-runs.
    f0 = frameio(dbg)
    dbg.send(f"sh write {PROBE_CONF} probe=2")
    f1 = wait_refresh(dbg, f0)
    check("an /etc write publishes a refresh", f1["refreshes"] > f0["refreshes"],
          f"{f0['refreshes']} -> {f1['refreshes']}")
    check("...which re-read cached files on the worker", f1["refresh_read"] > 0,
          f"{f1['refresh_read']} files, {f1['conf_cached']} cached")
    check("...and the WM's thread read no config file",
          f1["conf_reads"] == f0["conf_reads"], f"{f1['conf_reads'] - f0['conf_reads']} read")

    # 2. A setting the WM itself reads, back to its default.
    f0 = frameio(dbg)
    dbg.send("sh config set desktop.wallpaper.mode fill")
    f1 = wait_refresh(dbg, f0)
    check("a setting change publishes a refresh", f1["refreshes"] > f0["refreshes"])
    check("...and the WM's thread read no config file for it",
          f1["conf_reads"] == f0["conf_reads"], f"{f1['conf_reads'] - f0['conf_reads']} read")
    dbg.send(f"sh rm {PROBE_CONF}")


def wallpaper_check(dbg):
    first = frameio(dbg)["wallpaper"]
    other = "dusk" if first != "dusk" else "slate"
    dbg.send("gui watchdog 60")   # under a JPEG decode under TCG (~180 ms)
    dbg.logs(clear=True)
    dbg.send(f"sh config set desktop.wallpaper {other}")
    deadline = time.time() + 30
    while time.time() < deadline and frameio(dbg)["wallpaper"] != other:
        time.sleep(0.3)   # sleep-ok: the decode is a guest job, polled
    shown = frameio(dbg)["wallpaper"]
    check(f"the new wallpaper ({other}) is the one shown", shown == other, f"shown {shown!r}")
    lines = dbg.logs()
    took = [ln.strip() for ln in lines if f"desktop: wallpaper {other}" in ln]
    check("...decoded (the log names it)", bool(took), "; ".join(took)[:200])
    slow = [ln.strip() for ln in lines if "SLOW FRAME" in ln and "'wallpaper'" in ln]
    check("...and no frame was slow in 'wallpaper'", not slow, "; ".join(slow)[:300])
    dbg.send(f"sh config set desktop.wallpaper {first}")
    deadline = time.time() + 30
    while time.time() < deadline and frameio(dbg)["wallpaper"] != first:
        time.sleep(0.3)   # sleep-ok: as above
    dbg.send("gui watchdog 150")


def icons_check(dbg):
    def has(name):
        return any(name in ln for ln in dbg.send("gui apps").splitlines())

    name = "ZZ Frame IO probe"
    dbg.settle()
    f0 = frameio(dbg)
    dbg.send(f"sh write {ENTRY} Name={name}")
    dbg.send(f"sh append {ENTRY} Exec=/bin/wm/system/about")
    dbg.send(f"sh append {ENTRY} Icon=about")
    deadline = time.time() + 20
    while time.time() < deadline and not has(name):
        time.sleep(0.3)   # sleep-ok: the reload is debounced on the guest
    check("a new entry loads", has(name))
    dbg.settle()   # the install damages the whole desktop: every icon is drawn again
    f1 = frameio(dbg)
    check("...and the redrawn desktop decoded no icon on the WM's thread",
          f1["icon_decodes"] == f0["icon_decodes"], f"{f1['icon_decodes'] - f0['icon_decodes']} decoded")
    dbg.send(f"sh rm {ENTRY}")
    deadline = time.time() + 20
    while time.time() < deadline and has(name):
        time.sleep(0.3)   # sleep-ok: as above


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "frameio_test")
    if not args.in_gui:
        enter_gui(QMPSession(port=args.qmp_port), args.sock)
    dbg = DebugConsole(args.sock)
    print("frame I/O (config refresh, icons, wallpaper off the WM's thread)")
    f = frameio(dbg)
    if not check("`gui frameio` answers", "conf_reads" in f, str(f)[:200]):
        return _res.finish("frameio_test")
    settings_check(dbg)
    icons_check(dbg)
    wallpaper_check(dbg)
    return _res.finish("frameio_test")


if __name__ == "__main__":
    sys.exit(main())
