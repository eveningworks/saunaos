#!/usr/bin/env python3
"""A battery, an AC adapter and a lid, on a guest that has none of its own.

QEMU emulates none of the three, so this compiles the fixture SSDT
(tools/tests/fixtures/acpi/power.asl) with iasl, appends it to QEMU's
tables and boots a disk copy with `acpi=uacpi acpieval`; the fixture's
T* methods (`acpi --eval _SB.TSBT ...`) change the state and Notify()
as a laptop's EC query would, so the kernel's Notify path is what runs.

1. **The kernel reads them** -- `acpi` names the adapter, the battery's
   charge, energy, design, cycles and model, the lid; `_BTP` is armed at
   3% of the full charge.
2. **The tray and Quick Settings** -- the item shows the charge (a bolt
   while charging); unplugged, the summary says the time left; Quick
   Settings' footer opens the Battery page, whose bar is drawn to the
   charge (pixels: the accent inside it, the trough past it).
3. **Low** -- at 9% the icon goes amber and "Battery low" goes up, once.
4. **The lid** -- with `desktop.lid=screen` a close turns the screen off
   (QEMU refuses, which is the evidence it asked) and an open is input;
   with the default it LOCKS.
5. **Critical** -- at 3% on battery the kernel says so, waits its 20 s and
   sends init SIGPWR: the guest powers itself off.

THE POSITIVE CONTROL (--positive-control) boots WITHOUT the fixture
table: every battery and lid check must then go red.

On demand (ondemand_sweep.py): its own guest, and some 2-3 minutes.

    python3 tools/tests/sweep/power_test.py [--kvm] [--positive-control]
"""

RUNNER = {"covers": "batteries, the AC adapter and the lid: kernel facts, tray, Quick Settings, low, lid, critical",
          "needs": ["iasl", "needs iasl (ACPICA's ASL compiler: pacman -S acpica, apt install acpica-tools)"]}

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
REPO = os.path.dirname(HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from harness import Results, copy_disk          # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import install_grub                             # noqa: E402
import pixel_probe                              # noqa: E402
import port_guard                               # noqa: E402

ASL = os.path.join(HERE, "tests", "fixtures", "acpi", "power.asl")
WORDS = "acpi=uacpi acpieval"


def chk(res, ok, what):
    res.check(what, bool(ok), "")
    return ok


def battery(dbg):
    return dbg.json("gui battery --json") or {}


def wait_for(dbg, pred, timeout=20.0):
    """The tray re-reads once a second; the kernel moves on a Notify."""
    end = time.time() + timeout
    b = {}
    while time.time() < end:
        b = battery(dbg)
        if pred(b):
            return b
        dbg.settle()
    return b


def evaluate(dbg, path, *args):
    return dbg.send("sh acpi --eval " + path + "".join(f" {a}" for a in args)) or ""


def kernel_facts(dbg, res):
    out = dbg.send("sh acpi") or ""
    chk(res, "AC adapter:    plugged in" in out, "the kernel finds the AC adapter, plugged in")
    chk(res, "battery BAT0   78%, charging at 15.0 W, 29.8 of 38.2 Wh (design 45.0 Wh)" in out,
        "the kernel reads the battery: 78%, charging, its energy and design")
    chk(res, "SaunaOS fixture SAUNA-BAT, serial 0001, LION, 212 cycles" in out,
        "...its maker, model, serial, type and cycles (_BIX)")
    chk(res, "lid:           open" in out, "the kernel finds the lid, open")
    btp = evaluate(dbg, "_SB.TGBP")
    chk(res, "1146 (" in btp, f"_BTP is armed at 3% of the full charge, 1146 mWh ({btp.strip()[-30:]})")


def tray_and_card(dbg, qmp, res, tmp):
    b = wait_for(dbg, lambda b: b.get("batteries") == 1)
    chk(res, b.get("batteries") == 1 and not b.get("tray_hidden"), f"the tray shows a battery ({b})")
    chk(res, b.get("icon") == "tray-battery-charging-80" and b.get("charging"),
        f"charging at 78%: the 80 glyph with its bolt ({b.get('icon')})")
    evaluate(dbg, "_SB.TSAC", 0)
    evaluate(dbg, "_SB.TSBT", 1, 6100, 29800)
    b = wait_for(dbg, lambda b: b.get("discharging") and "left" in b.get("summary", ""))
    chk(res, b.get("summary") == "78% · 4 h 53 min left" and b.get("icon") == "tray-battery-80",
        f"unplugged: '78% · 4 h 53 min left', no bolt ({b.get('summary')!r}, {b.get('icon')})")

    q = dbg.json("gui quick --json") or {}
    bx, by, bw, bh = q.get("button", [0, 0, 0, 0])
    dbg.click(bx + bw // 2, by + bh // 2)
    q = dbg.json("gui quick --json") or {}
    x, y, w, h = q.get("battery", [0, 0, 0, 0])
    if not chk(res, q.get("open") and w > 0, f"Quick Settings' footer carries the charge ({q.get('battery')})"):
        return
    dbg.click(x + w // 2, y + h // 2)
    q = dbg.json("gui quick --json") or {}
    b = battery(dbg)
    chk(res, q.get("page") == "battery" and b.get("open"), f"the footer opens the Battery page ({q.get('page')})")
    bar = b.get("bar", [0, 0, 0, 0])
    png = os.path.join(tmp, "card.png")
    qmp.screenshot(png)
    mid_y = bar[1] + bar[3] // 2
    from PIL import Image
    im = Image.open(png).convert("RGB")
    inside = pixel_probe.sample(im, bar[0] + bar[2] * 70 // 100, mid_y, 1)
    past = pixel_probe.sample(im, bar[0] + bar[2] * 90 // 100, mid_y, 1)
    chk(res, inside != past and inside[2] > inside[0],
        f"the bar is filled to the charge: accent at 70%, trough at 90% ({inside} vs {past})")
    dbg.key("esc")
    dbg.click(5, 5)   # light-dismiss whatever is still open


def low(dbg, res):
    dbg.logs("wm: battery low", clear=True)
    evaluate(dbg, "_SB.TSBT", 1, 6100, 3400)
    b = wait_for(dbg, lambda b: b.get("warned"))
    chk(res, b.get("warned") and b.get("ink") and b.get("icon") == "tray-battery-10",
        f"at 9%: amber, one bar, the low notice ({b.get('percent')}%, ink {b.get('ink')}, {b.get('icon')})")
    evaluate(dbg, "_SB.TSBT", 1, 6100, 3300)
    wait_for(dbg, lambda b: b.get("percent") == 9 and b.get("seconds", 0) < 7000)
    said = dbg.logs("wm: battery low", clear=True)
    chk(res, len(said) == 1, f"the notice goes up once per discharge ({len(said)})")


def lid(dbg, res):
    dbg.send("sh config set desktop.lid screen")
    dbg.settle()
    evaluate(dbg, "_SB.TSLD", 0)
    b = wait_for(dbg, lambda b: b.get("lid", {}).get("acted") == 1)
    chk(res, b.get("lid", {}).get("last") == "screen", f"desktop.lid=screen: a close turns the screen off ({b.get('lid')})")
    asked = [l for l in dbg.logs("wm: ", clear=True) if "screen" in l]
    chk(res, any("cannot be turned off" in l or "screen off" in l for l in asked),
        f"...and the kernel was asked ({asked[-2:]})")
    evaluate(dbg, "_SB.TSLD", 1)
    b = wait_for(dbg, lambda b: b.get("lid", {}).get("open"))
    opened = [l for l in dbg.logs("wm: ", clear=True) if "the lid opened" in l]
    chk(res, b.get("lid", {}).get("open") and opened, f"an open is seen and acted on ({opened[-1:]})")
    dbg.send("sh config set desktop.lid lock")
    dbg.settle()
    evaluate(dbg, "_SB.TSLD", 0)
    b = wait_for(dbg, lambda b: b.get("lid", {}).get("acted") == 2)
    login = dbg.json("gui login") or {}
    chk(res, b.get("lid", {}).get("last") == "lock" and login.get("open"),
        f"the default: a close LOCKS ({b.get('lid')}, lock screen {login.get('open')})")
    evaluate(dbg, "_SB.TSLD", 1)


def critical(vm, dbg, res):
    evaluate(dbg, "_SB.TSBT", 1, 6100, 1000)
    end = time.time() + 75
    said, off = "", False
    while time.time() < end:
        st = vm("status")
        if "not running" in st or "stopped" in st:
            off = True
            break
        try:
            said = dbg.send("sh dmesg -n 40") or said
        except Exception:  # noqa: BLE001 -- the guest went away mid-read
            pass
        time.sleep(2)  # sleep-ok: waits on the guest powering itself off, which has no event to wait on
    chk(res, "the battery is critical" in said or off, "at 3% on battery the kernel says it is critical")
    chk(res, off, "...and 20 s later the guest powers itself off (SIGPWR to init)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="boot without the fixture table: the battery and lid checks must fail")
    ap.add_argument("--keep", metavar="DIR", help="keep the image, the table and the screenshots here")
    args = ap.parse_args()
    if not shutil.which("iasl"):
        print("power_test: SKIP -- no iasl on PATH (pacman -S acpica)")
        return 0
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    tmp = args.keep or tempfile.mkdtemp(prefix="power-")
    os.makedirs(tmp, exist_ok=True)
    try:
        return run_in(args, tmp)
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)


def run_in(args, tmp):
    res = Results()
    r = subprocess.run(["iasl", "-p", os.path.join(tmp, "power"), ASL], capture_output=True, text=True)
    aml = os.path.join(tmp, "power.aml")
    if not chk(res, r.returncode == 0 and os.path.exists(aml), "iasl compiles the fixture"):
        print(r.stdout[-800:], r.stderr[-800:])
        return 1
    disk = os.path.join(tmp, "disk.img")
    copy_disk("disk.img", disk, cwd=REPO)
    ok, why = install_grub.add_boot_word(disk, WORDS)
    if not chk(res, ok, f"the copy boots with {WORDS} {why}"):
        return 1
    table = [] if args.positive_control else ["--acpi-table", aml]

    def vm(*argv):
        p = subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(args.instance),
                            "--disk", disk, *table, *(["--kvm"] if args.kvm else []), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False, timeout=300)
        return p.stdout + p.stderr

    vm("stop")
    if not chk(res, "ready" in vm("start"), "the guest came up"):
        return 1
    try:
        qmp = QMPSession(port=port_guard.instance_qmp(args.instance))
        sock = port_guard.instance_sock(args.instance)
        enter_gui(qmp, sock)
        dbg = DebugConsole(sock)
        try:
            kernel_facts(dbg, res)
            tray_and_card(dbg, qmp, res, tmp)
            low(dbg, res)
            lid(dbg, res)
            critical(vm, dbg, res)
        finally:
            dbg.close()
    finally:
        vm("stop")
    print(f"\npower_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
