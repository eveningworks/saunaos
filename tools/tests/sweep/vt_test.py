#!/usr/bin/env python3
"""Virtual terminals on Ctrl+Alt+F1..F4, on a `text` boot.

WHAT IS UNDER TEST
------------------
docs/tty-design.md, "The virtual terminals". Each check names a failure
the others would not catch:

1. **A switch shows another screen.** Ctrl+Alt+F2 logs `vt: tty2 shown`
   and the screen stops being VT1's.

2. **VT1 comes back EXACTLY, cursor-addressed paint included.** The
   numbered rows /tests/ansiregion places with cursor moves and a
   scrolling region are redrawn from VT1's cell GRID. A console that
   rebuilt the screen from its scrollback -- the byte stream, which is
   all it had before VTs -- puts them back in the wrong places.

3. **Keys go to the VT on screen.** Typed on VT2, they change VT2 and
   leave VT1 untouched.

3a. **A VT gets a shell the first time it is shown, and keeps one.**
   init starts `autovt@tty2` (the `TTY=vt` template) on the first switch;
   a line typed there RUNS -- `touch` makes a file the debug console then
   finds, which nothing but a shell reading VT2 can do. `exit` brings a
   new shell on a CLEARED screen: the frame is pixel-identical to the
   first fresh one, so neither the old commands nor a second banner can
   be on it.

4. **Output follows its terminal, not the screen.** A program writing to
   tty0 while VT2 is shown changes nothing on VT2, and its line is on
   VT1 after the switch back.

5. **Scrollback is per VT.** Shift+PageUp on VT1 shows its history and
   Shift+PageDown returns to the identical live screen; on VT2 it shows
   VT2's few lines, not VT1's boot log.

Every frame compared is a SETTLED one (QMPSession.screenshot()), and
every wait is on the state asserted -- a log line, a process gone, a
frame that changed -- never a fixed sleep.

Usage:
    python3 tools/tests/sweep/vt_test.py
    python3 tools/tests/sweep/vt_test.py --instance 3

Exits 0 if every check passed, 1 otherwise, 2 if it could not run.
"""

RUNNER = {"name": "vt", "covers": "virtual terminals: switch, per-VT screen, keys, output and scrollback"}

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
REPO = toolpath.REPO
from gui_debug import DebugConsole      # noqa: E402
import vm as vm_mod                     # noqa: E402 -- started_ok()
from shell_flow import ShellFlow        # noqa: E402
import port_guard                       # noqa: E402
from harness import Results, copy_disk, poll  # noqa: E402
from PIL import Image, ImageChops       # noqa: E402

VM = toolpath.tool("vm.py")
# The console's cell height at the test guest's 1280x720 with the default
# font: 45 rows. A wrong value fails loudly (every band reads blank).
CELL_H = 16

_res = Results()
check = _res.check


def vm_run(disk, instance, *argv):
    cmd = [sys.executable, VM, "--disk", disk]
    if instance:
        cmd += ["--instance", str(instance)]
    r = subprocess.run(cmd + list(argv), cwd=REPO, capture_output=True, text=True)
    return r.stdout + r.stderr


def dmesg(dbg):
    prev = dbg.timeout
    try:
        dbg.timeout = 20.0
        return dbg.send("sh dmesg") or ""
    finally:
        dbg.timeout = prev


def running(dbg, name):
    """Is a process called `name` in the scheduler's table?"""
    out = dbg.send("sh kstack slots") or ""
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[0].isdigit() and p[2].startswith(name):
            return True
    return False


class Screen:
    def __init__(self, q, scratch):
        self.q, self.scratch, self.n = q, scratch, 0

    def grab(self):
        self.n += 1
        path = os.path.join(self.scratch, f"vt{self.n:02d}.png")
        self.q.screenshot(path)
        return Image.open(path).convert("RGB")

    def changed_from(self, ref, box=None, timeout=10.0):
        """The first settled frame that differs from `ref` (in `box`),
        or the last one taken."""
        last = [None]

        def differs():
            last[0] = self.grab()
            return not same(last[0], ref, box)
        poll(differs, timeout, step=0.3)
        return last[0]

    def steady(self, ref, timeout=15.0):
        """A frame that differs from `ref` AND equals the one before it --
        what a shell's answer looks like once it has finished arriving."""
        prev, last = [None], [None]

        def done():
            prev[0], last[0] = last[0], self.grab()
            return (prev[0] is not None and not same(last[0], ref)
                    and same(last[0], prev[0]))
        poll(done, timeout, step=0.3)
        return last[0]


def same(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return ImageChops.difference(a, b).getbbox() is None


def lit(img, box=None):
    """Pixels that are not the console's black, in `box`."""
    if box:
        img = img.crop(box)
    return sum(1 for p in img.get_flattened_data() if p != (0, 0, 0))


def switch(q, dbg, n, seen):
    """Ctrl+Alt+F<n>, then wait for the kernel to say it was shown -- the
    `vt` thread logs it after the redraw. `seen` counts lines already
    there, since a VT shown twice logs twice."""
    marker = f"vt: tty{n} shown"
    q.combo(["ctrl", "alt", f"f{n}"])
    ok = poll(lambda: dmesg(dbg).count(marker) > seen.get(n, 0), 15.0, step=0.5)
    seen[n] = seen.get(n, 0) + 1
    return bool(ok)


def type_text(q, text):
    for ch in text:
        q.send_key("spc" if ch == " " else ch)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--instance", default="auto",
                    help="slot number, or `auto` for the lowest free one")
    ap.add_argument("--keep", action="store_true", help="leave the VM running")
    args = ap.parse_args()
    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("vt_test: no free VM slot")
            return 2
        args.instance = n
        print(f"vt_test: slot {n} (QMP {4445 + n})")
    args.instance = int(args.instance)

    scratch = tempfile.mkdtemp(prefix="vt_test.")
    if not args.disk:
        args.disk = copy_disk(os.path.join(REPO, "disk.img"), os.path.join(scratch, "disk.img"))
    sock = ".vm.serial" if not args.instance else f".vm.{args.instance}.serial"
    port = 4445 + args.instance

    try:
        print("first boot -- configuring the target")
        if not vm_mod.started_ok(vm_run(args.disk, args.instance, "start")):
            print("vt_test: could not start the VM")
            return 2
        dbg = DebugConsole(sock)
        dbg.send("sh keyboard us")
        dbg.send("sh config set system.default_target text")
        vm_run(args.disk, args.instance, "stop")

        print("second boot -- the one under test")
        if not vm_mod.started_ok(vm_run(args.disk, args.instance, "start")):
            print("vt_test: the text-target boot never became ready")
            return 2
        dbg = DebugConsole(sock)
        flow = ShellFlow(qmp_port=port)   # QEMU serves ONE QMP client: share it
        q = flow.session
        scr = Screen(q, scratch)
        seen = {}
        if not poll(lambda: "init: started tosh" in dmesg(dbg), 30.0, step=0.5):
            print("vt_test: tosh never started on the console")
            return 2

        w, h = scr.grab().size

        print("VT1: a cursor-addressed screen")
        dbg.send("sh spawn /tests/ansiregion")
        started = poll(lambda: running(dbg, "ansiregion"), 10.0)
        gone = started and poll(lambda: not running(dbg, "ansiregion"), 30.0, step=0.5)
        check("ansiregion painted VT1 and exited", bool(gone))
        vt1 = scr.grab()

        print("Ctrl+Alt+F2")
        check("Ctrl+Alt+F2 shows tty2", switch(q, dbg, 2, seen))
        started = poll(lambda: "init: started autovt@tty2" in dmesg(dbg), 15.0, step=0.5)
        check("init started a shell on tty2", bool(started))
        # The prompt is the fourth row: banner, blank, tosh's line, prompt.
        prompt = (0, 3 * CELL_H, w, 4 * CELL_H)
        poll(lambda: lit(scr.grab(), prompt) > 0, 15.0, step=0.5)
        vt2 = scr.grab()
        check("the screen is no longer VT1's", not same(vt2, vt1),
              f"{lit(vt2)} lit pixels on VT2, {lit(vt1)} on VT1")
        check("VT2 shows the banner and a prompt",
              lit(vt2, (0, 0, w, CELL_H)) > 0 and lit(vt2, prompt) > 0)

        print("a line typed on VT2 runs")
        flow.type_command("touch /vtprobe.txt")
        q.send_key("ret")
        made = poll(lambda: "vtprobe.txt" in (dbg.send("sh ls /") or ""), 15.0, step=0.5)
        check("a command typed on VT2 ran", bool(made))
        dbg.send("sh rm /vtprobe.txt")

        print("exit, and a fresh shell on a cleared screen")
        flow.type_command("exit")
        q.send_key("ret")
        again = poll(lambda: dmesg(dbg).count("init: started autovt@tty2") >= 2, 15.0, step=0.5)
        check("init started tty2's shell again after exit", bool(again))
        poll(lambda: same(scr.grab(), vt2), 15.0, step=0.5)
        fresh = scr.grab()
        check("the new shell's screen is the first one's, cleared", same(fresh, vt2))

        print("keys go to the VT on screen")
        type_text(q, "zzz")
        q.send_key("ret")
        type_text(q, "yyy")
        q.send_key("ret")
        vt2b = scr.steady(vt2)
        check("typing on VT2 changes VT2", not same(vt2b, vt2))

        print("output to tty0 while VT2 is shown")
        reply = dbg.send("sh spawn /bin/echo vtmarker") or ""
        echoed = "started as pid" in reply and poll(lambda: not running(dbg, "echo"), 10.0)
        vt2c = scr.grab()
        check("a write to tty0 leaves VT2's screen alone", bool(echoed) and same(vt2c, vt2b))

        print("Ctrl+Alt+F1")
        check("Ctrl+Alt+F1 shows tty1", switch(q, dbg, 1, seen))
        back = scr.grab()
        # ansiregion's rows are in the top 20; the echo's line lands
        # below them.
        top = (0, 0, w, 20 * CELL_H)
        check("VT1's cursor-addressed rows come back exactly", same(back, vt1, top))
        check("VT1 got the line written while it was hidden", not same(back, vt1))

        print("scrollback is per VT")
        q.combo(["shift", "pgup"])
        hist = scr.changed_from(back)
        check("Shift+PageUp shows VT1's history", not same(hist, back))
        q.combo(["shift", "pgdn"])
        live = scr.changed_from(hist)
        check("Shift+PageDown returns to the identical live VT1", same(live, back))

        check("back on tty2", switch(q, dbg, 2, seen))
        vt2d = scr.grab()
        check("VT2 is as it was left", same(vt2d, vt2b))
        q.combo(["shift", "pgup"])
        hist2 = scr.changed_from(vt2d, timeout=4.0)
        check("VT2's history is VT2's, not VT1's boot log",
              lit(hist2) < lit(hist) // 4,
              f"{lit(hist2)} lit pixels on VT2's history, {lit(hist)} on VT1's")
        q.combo(["shift", "pgdn"])
        scr.changed_from(hist2, timeout=4.0)
    finally:
        if not args.keep:
            vm_run(args.disk, args.instance, "stop")

    return _res.finish("vt_test")


if __name__ == "__main__":
    sys.exit(main())
