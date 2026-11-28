#!/usr/bin/env python3
"""A thermal zone and the _PRW wake set, on a guest that has neither.

QEMU emulates no thermal zone and no _PRW, so this compiles the fixture
SSDT (tools/tests/fixtures/acpi/power.asl) with iasl, appends it to
QEMU's tables and boots a disk copy with `acpi=uacpi acpieval`; the
fixture's `_SB.TSTZ <C>` sets the zone's temperature and Notify()s, as
the ASUS's EC query does, so the kernel's Notify path is what runs.

1. **The kernel reads the zone** -- `acpi` shows 50 C with its critical
   100 and passive 80 C; a Notify moves the reading.
2. **The wake set** -- `acpi --wake` lists the lid (S5, armed) and the
   S3-only device (not armed); the runtime GPEs are enabled.
3. **Task Manager** -- a Temperature device under CPU, its chart carrying
   the trips as reference lines and the zone as a line.
4. **Passive cooling** -- over 80 C the kernel slows the CPUs (not applied
   in a guest, and says why), "Running hot" goes up once; under it again
   the spell ends.
5. **Too hot** -- within 5 C of critical the critical notice goes up, and
   comes down when it cools. With `desktop.hot_notices=off` neither notice
   goes up while the kernel still slows the CPUs.
6. **Critical** -- at 100 C, two readings a second apart, the kernel sends
   init SIGPWR: the guest powers itself off.

THE POSITIVE CONTROL (--positive-control) boots WITHOUT the fixture
table: every check after the boot must then go red, bar three that are
not the fixture's -- the runtime GPEs (QEMU's own _GPE), Task Manager
opening and listing its devices.

On demand (ondemand_sweep.py): its own guest, and some 2 minutes.

    python3 tools/tests/sweep/thermal_test.py [--kvm] [--positive-control]
"""

RUNNER = {"covers": "thermal zones and the _PRW wake set: kernel facts, Task Manager, passive, hot notices, critical",
          "needs": ["iasl", "needs iasl (ACPICA's ASL compiler: pacman -S acpica, apt install acpica-tools)"]}

import argparse
import os
import re
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
import port_guard                               # noqa: E402
import taskmgr_test as tm                       # noqa: E402 -- its layout reader

ASL = os.path.join(HERE, "tests", "fixtures", "acpi", "power.asl")
WORDS = "acpi=uacpi acpieval"
ZONE = re.compile(r"\\_TZ_\.TZ0_\s+(-?\d+) C\s+highest (-?\d+) C\s+critical (\S+) C\s+passive (\S+) C")


def chk(res, ok, what):
    res.check(what, bool(ok), "")
    return ok


def evaluate(dbg, path, *args):
    return dbg.send("sh acpi --eval " + path + "".join(f" {a}" for a in args)) or ""


def zone(dbg):
    m = ZONE.search(dbg.send("sh acpi") or "")
    return (int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)) if m else None


def thermal(dbg):
    return dbg.json("gui thermal --json") or {}


def wait(pred, read, timeout=15.0):
    """Wait on what is asserted: the zone is read on its Notify, the WM
    looks once a second."""
    end = time.time() + timeout
    v = read()
    while not pred(v) and time.time() < end:
        time.sleep(0.25)  # sleep-ok: polls the guest's own answer, which has no event to wait on
        v = read()
    return v


def kernel_facts(dbg, res):
    z = zone(dbg)
    chk(res, z and z[0] == 50 and z[2] == "100" and z[3] == "80",
        f"the kernel reads the zone: 50 C, critical 100, passive 80 ({z})")
    evaluate(dbg, "_SB.TSTZ", 70)
    z = wait(lambda z: z and z[0] == 70, lambda: zone(dbg))
    chk(res, z and z[0] == 70 and z[1] == 70, f"a Notify(0x80) moves the reading to 70 C, the highest with it ({z})")
    wake = dbg.send("sh acpi --wake") or ""
    chk(res, re.search(r"\\_SB_\.LID0\s+PNP0C0D\s+0x0d\s+S5\s+armed", wake),
        "acpi --wake: the lid wakes from S5 and is armed")
    chk(res, re.search(r"\\_SB_\.WAK0\s+PNP0C02\s+0x0e\s+S3\s+-", wake),
        "acpi --wake: the S3-only device is listed, not armed")
    log = dbg.send("sh dmesg") or ""
    chk(res, "runtime GPEs enabled" in log, "the runtime GPEs are enabled")


def task_manager(dbg, qmp, res, tmp):
    dbg.spawn(tm.TASKMGR, "Task Manager")
    tm.wait_layout(dbg, lambda l: "rail" in l and "rail.row_h" in l)
    win = tm.window(dbg)
    if not chk(res, win, "Task Manager opened"):
        return
    cx, cy = win["content"]["x"], win["content"]["y"]
    rail, rh = tm._layout["rail"], tm._layout["rail.row_h"]
    dbg.click(cx + rail[0] + rail[2] // 2, cy + rail[1] + rh + rh // 2)
    lay = tm.wait_layout(dbg, lambda l: l.get("page") == 1 and "devices" in l and "devices.item_h" in l)
    dv, ih = lay.get("devices"), lay.get("devices.item_h")
    if not chk(res, dv and ih, "the Performance page lists its devices"):
        return
    dbg.click(cx + dv[0] + dv[2] // 2, cy + dv[1] + ih + ih // 2)
    lay = tm.wait_layout(dbg, lambda l: l.get("device", (0,))[0] == 1)
    chk(res, lay.get("device") == (1, "Temperature"), f"Temperature is the device under CPU ({lay.get('device')})")
    tm.wait_layout(dbg, lambda l: any("perfchart.ref" in x for x in tm._lines))
    refs = " | ".join(x for x in tm._lines if "perfchart.ref" in x)
    chk(res, "ref 0 100 Shuts down 100 °C" in refs and "ref 1 80 Slows down 80 °C" in refs,
        f"its chart carries the trips as reference lines ({refs[-120:]})")
    tm.wait_layout(dbg, lambda l: any("perfchart.legend_value 0 70 °C" in x for x in tm._lines))
    chk(res, any("perfchart.legend_value 0 70 °C" in x for x in tm._lines),
        "...and the zone as a line, its legend reading 70 °C")
    qmp.screenshot(os.path.join(tmp, "temperature.png"))
    print(f"  screenshot: {os.path.join(tmp, 'temperature.png')}")


def passive_and_notices(dbg, res):
    evaluate(dbg, "_SB.TSTZ", 85)
    t = wait(lambda t: t.get("slowing") and t.get("hot_notice"), lambda: thermal(dbg))
    chk(res, t.get("slowing"), f"over 80 C the kernel is slowing the CPUs ({t})")
    acpi = dbg.send("sh acpi") or ""
    chk(res, "slowing the CPUs to" in acpi and "not applied: a virtual machine" in acpi,
        "...not applied in a guest, and acpi says why")
    state = dbg.json("gui state --json") or {}
    chk(res, t.get("hot_notice") and state.get("notice", {}).get("title") == "Running hot",
        f"'Running hot' goes up ({state.get('notice', {}).get('title')})")
    evaluate(dbg, "_SB.TSTZ", 96)
    t = wait(lambda t: t.get("too_hot_notice"), lambda: thermal(dbg))
    state = dbg.json("gui state --json") or {}
    chk(res, t.get("too_hot_notice") and state.get("notice", {}).get("title") == "The computer is too hot",
        f"within 5 C of critical, 'The computer is too hot' goes up ({state.get('notice', {}).get('title')})")
    evaluate(dbg, "_SB.TSTZ", 60)
    t = wait(lambda t: not t.get("too_hot_notice") and not t.get("slowing"), lambda: thermal(dbg), 30)
    chk(res, t.get("zones") and not t.get("too_hot_notice"), "cooled, the too-hot notice comes down")
    chk(res, t.get("zones") and not t.get("slowing"), f"...and the passive spell ends ({t})")


def notices_off(dbg, res):
    """The setting off: the trips crossed again and no card raised; back on
    while still near critical, "too hot" -- a state, not news -- goes up."""
    def raised():
        log = dbg.send("sh dmesg") or ""
        return log.count("wm: running hot"), log.count("wm: too hot")
    dbg.send("gui spawn /bin/config set desktop.hot_notices off")
    t = wait(lambda t: t.get("enabled") is False, lambda: thermal(dbg))
    if not chk(res, t.get("zones") and t.get("enabled") is False,
               f"desktop.hot_notices=off reaches the desktop ({t.get('enabled')})"):
        return
    before = raised()
    evaluate(dbg, "_SB.TSTZ", 97)
    t = wait(lambda t: t.get("slowing") and t.get("near_critical"), lambda: thermal(dbg))
    time.sleep(2)  # sleep-ok: two of the WM's once-a-second polls, for a card that must NOT come
    after = raised()
    t = thermal(dbg)
    chk(res, t.get("slowing") and t.get("near_critical") and before[0] > 0 and after == before
        and not t.get("hot_notice") and not t.get("too_hot_notice"),
        f"off: the kernel still slows the CPUs near critical, and neither notice goes up ({before} -> {after})")
    dbg.send("gui spawn /bin/config set desktop.hot_notices on")
    t = wait(lambda t: t.get("too_hot_notice"), lambda: thermal(dbg))
    state = dbg.json("gui state --json") or {}
    chk(res, t.get("too_hot_notice") and state.get("notice", {}).get("title") == "The computer is too hot",
        "back on while still near critical, 'too hot' goes up at once")
    evaluate(dbg, "_SB.TSTZ", 60)
    wait(lambda t: not t.get("slowing") and not t.get("too_hot_notice"), lambda: thermal(dbg), 30)


def critical(vm, dbg, res):
    evaluate(dbg, "_SB.TSTZ", 101)
    end = time.time() + 30
    off = False
    while time.time() < end:
        st = vm("status")
        if "not running" in st or "stopped" in st:
            off = True
            break
        time.sleep(1)  # sleep-ok: waits on the guest powering itself off, which has no event to wait on
    chk(res, off, "at its critical trip, read twice, the guest powers itself off (SIGPWR to init)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="boot without the fixture table: every check after the boot must fail")
    ap.add_argument("--keep", metavar="DIR", help="keep the image, the table and the screenshot here")
    args = ap.parse_args()
    if not shutil.which("iasl"):
        print("thermal_test: SKIP -- no iasl on PATH (pacman -S acpica)")
        return 0
    if args.instance is None:
        args.instance = port_guard.find_free_instance()
    tmp = args.keep or tempfile.mkdtemp(prefix="thermal-")
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
            task_manager(dbg, qmp, res, tmp)
            passive_and_notices(dbg, res)
            notices_off(dbg, res)
            critical(vm, dbg, res)
        finally:
            dbg.close()
    finally:
        vm("stop")
    print(f"\nthermal_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
