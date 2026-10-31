#!/usr/bin/env python3
"""Whether toywm's frames land on the vblank, measured on a machine whose
display can say where the beam is (Intel: QUERY_VBLANK) -- the one
place pacing to the vblank does anything, since QEMU's displays cannot.

Each run drags a Notepad window by its title bar with `guictl drag ...
STEPS HZ` (moves paced as a mouse reports them, 250 Hz by default) and
reads `guictl compositor --json` after: frames the panel SHOWED, frames
replaced before a vblank showed them, and the lag from a change being
owed to the vblank that showed it. Runs alternate `gui pacing refresh`
(one frame a refresh, phase unknown) and `vblank` (each frame started
just before its vblank) on the same machine and window, so the refresh
runs are the control: they skip refreshes that the vblank runs must not.

  python3 tools/vblank_pacing.py --host <asus-ip>
  python3 tools/vblank_pacing.py --host <asus-ip> --runs 8 --hz 125

On demand, against a machine you name; it opens a Notepad window there
(or uses the one it finds), moves it, and leaves pacing on.
"""
import argparse
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRAG_PX = 400
SECONDS = 2.0


def exec_on(host, *cmds):
    r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "remote.py"), "--host", host,
                        "--timeout", "30", "exec", *cmds],
                       capture_output=True, text=True, timeout=180)
    return r.stdout


def notepad(host):
    """Notepad's window as (x, y, w, h), opened and restored if need be."""
    def find():
        for line in exec_on(host, "guictl windows --json").splitlines():
            if line.startswith("{"):
                for w in json.loads(line).get("windows", []):
                    if w.get("app_id") == "notepad":
                        return w
        return None
    w = find()
    if not w:
        exec_on(host, "guictl spawn /bin/wm/apps/notepad", "sleep 3")
        w = find()
    if w and w.get("state") == "maximized":
        # Pulled down by its title bar, as a hand restores it.
        cx = w["x"] + w["w"] // 2
        exec_on(host, f"guictl drag {cx} 12 {cx - 60} 300 80 250", "sleep 2")
        w = find()
    return w


def run_once(host, mode, x0, x1, y, hz):
    steps = int(SECONDS * hz)
    out = exec_on(host, f"guictl pacing {mode}", "guictl compositor reset",
                  f"guictl drag {x0} {y} {x1} {y} {steps} {hz}",
                  f"sleep {int(SECONDS) + 1}", "guictl compositor --json")
    js = [l for l in out.splitlines() if l.startswith("{")]
    return json.loads(js[-1]) if js else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True, help="the machine (local_info.txt)")
    ap.add_argument("--runs", type=int, default=4, help="runs per mode (default 4)")
    ap.add_argument("--hz", type=int, default=250, help="the drag's input rate (default 250)")
    args = ap.parse_args()

    w = notepad(args.host)
    if not w:
        print("vblank_pacing: no Notepad window to drag")
        return 1
    y = w["y"] + 12
    xa = w["x"] + w["w"] // 2
    xb = xa + DRAG_PX

    probe = run_once(args.host, "vblank", xa, xb, y, args.hz)
    if not probe or not probe.get("vblank", {}).get("known"):
        print("vblank_pacing: this display cannot say where the beam is -- nothing to measure")
        return 77
    period_us = probe["vblank"]["period_us"]
    refreshes = SECONDS * 1e6 / period_us
    print(f"vblank_pacing: a refresh every {period_us} us; {SECONDS:.0f} s drags at {args.hz} Hz "
          f"have room for {refreshes:.0f} frames")

    res = {"refresh": [], "vblank": []}
    order = ["refresh", "vblank", "vblank", "refresh"]
    for i in range(args.runs * 2):
        mode = order[i % 4]
        x0, x1 = (xb, xa) if i % 2 == 0 else (xa, xb)   # the probe left it at xb
        c = run_once(args.host, mode, x0, x1, y, args.hz)
        if not c:
            print(f"  {mode:8s} no reply")
            continue
        v = c["vblank"]
        res[mode].append(v)
        print(f"  {mode:8s} shown {v['shown']:4d}  replaced {v['dropped']:3d}  "
              f"change -> shown p50 {v['lag_p50'] / 1000:5.1f} p99 {v['lag_p99'] / 1000:5.1f} ms  "
              f"a frame costs {v['render_us'] / 1000:.1f} ms")
    exec_on(args.host, "guictl pacing on")

    def worst(mode, key, fn):
        vals = [v[key] for v in res[mode]]
        return fn(vals) if vals else None
    fails = 0
    def check(name, ok, detail):
        nonlocal fails
        print(f"  {'PASS' if ok else 'FAIL'}  {name}  ({detail})")
        fails += 0 if ok else 1

    vb_shown = worst("vblank", "shown", min)
    rf_shown = worst("refresh", "shown", min)
    check("paced to the vblank, a drag is shown on every refresh but at most one",
          vb_shown is not None and vb_shown >= refreshes - 1,
          f"fewest shown {vb_shown} of {refreshes:.0f}")
    check("...and no frame is replaced before a vblank shows it",
          worst("vblank", "dropped", max) == 0, f"most replaced {worst('vblank', 'dropped', max)}")
    vb_p99 = worst("vblank", "lag_p99", max)
    rf_p99 = worst("refresh", "lag_p99", min)
    check("...with a lower worst lag than pacing to the refresh alone",
          vb_p99 is not None and rf_p99 is not None and vb_p99 < rf_p99,
          f"vblank's worst p99 {vb_p99 and vb_p99 / 1000:.1f} ms, refresh's best "
          f"{rf_p99 and rf_p99 / 1000:.1f} ms; refresh showed as few as {rf_shown}")
    print(f"vblank_pacing: {3 - fails} passed, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
