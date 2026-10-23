#!/usr/bin/env python3
"""More devices than the old fixed tables held: PCI past 32, USB past 8.

The PCI table was `g_devices[32]` and dropped every device after the
32nd without a word; USB stopped at eight -- the device table and the
xHCI slots the controller was told to enable were both eight. A real
PC passes 32 PCI functions easily and a hub or two passes eight USB
devices, but QEMU's default machine has a handful of each, so nothing
else here ever reached either limit.

Two phases, each its own guest on a COPY of disk.img:

  pci  `vm.py --pci-fill 40`: forty inert pci-testdev devices behind two
       bridges. The kernel's count must pass 32, `lspci` must print a
       row for every device the kernel counted, and the last bridge's
       devices must be among them.
  usb  `vm.py --usb xhci+crowd`: a hub of eight keyboards, a mouse and a
       tablet -- eleven devices. All eleven must enumerate, and the HID
       table, which stays fixed (the interrupt path walks it), must SAY
       it is full rather than skip the extra keyboards silently.

`--positive-control` is not a flag: build HEAD~ (or any build before the
change) and run this against it -- the pci phase stops at 32 and the
usb phase at eight.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402
import vm as vm_mod  # noqa: E402
from gui_debug import DebugConsole  # noqa: E402
from harness import copy_disk  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCI_FILL = 40
USB_CROWD = 11   # vm.py's xhci+crowd: hub + 8 keyboards + mouse + tablet

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail else ""))
    return ok


class Guest:
    """One booted guest with extra devices, on a copy of disk.img."""

    def __init__(self, instance, extra):
        self.instance = instance
        self.extra = extra
        t = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        t.close()
        self.disk = t.name
        copy_disk(os.path.join(REPO, "disk.img"), self.disk)

    def _run(self, *args):
        cmd = [sys.executable, os.path.join(REPO, "tools", "vm.py"),
               "--disk", self.disk, "--instance", str(self.instance)] + self.extra + list(args)
        return subprocess.run(cmd, capture_output=True, text=True, cwd=REPO).stdout

    def start(self):
        return vm_mod.started_ok(self._run("start"))

    def cleanup(self):
        self._run("stop")
        try:
            os.unlink(self.disk)
        except OSError:
            pass

    def console(self):
        sock = ".vm.serial" if not self.instance else f".vm.{self.instance}.serial"
        return DebugConsole(os.path.join(REPO, sock))


def phase_pci(instance):
    print(f"\nphase pci: {PCI_FILL} extra devices behind bridges")
    g = Guest(instance, ["--pci-fill", str(PCI_FILL)])
    try:
        if not g.start():
            return check("guest boots with the extra devices", False)
        d = g.console()
        log = d.send("sh dmesg") or ""
        m = re.search(r"pci: (\d+) device\(s\) found", log)
        found = int(m.group(1)) if m else 0
        check("the kernel records more than 32 devices", found > 32,
              m.group(0) if m else "no count line")
        check("...including every one QEMU was given", found >= PCI_FILL)
        check("nothing was dropped for want of memory", "not recorded" not in log)

        out = d.send("sh lspci") or ""
        rows = re.findall(r"^([0-9a-f]{2}):([0-9a-f]{2})\.([0-7])  ", out, re.M)
        check("lspci prints a row for every device the kernel counted",
              len(rows) == found, f"{len(rows)} rows, {found} counted")
        # pci-testdev is 1b36:0005; the second bridge holds the last nine.
        testdevs = out.count("1b36:0005")
        check(f"all {PCI_FILL} pci-testdev devices are listed", testdevs == PCI_FILL,
              f"{testdevs} listed")
        buses = {int(b, 16) for b, _, _ in rows}
        check("devices behind both bridges are listed", len(buses) >= 3,
              f"buses {sorted(buses)}")
        kout = d.send("sh lspci -k") or ""
        check("lspci -k names an owner for every row",
              len(re.findall(r"kernel driver: |no driver|claimed by pid", kout)) == found)
        return True
    finally:
        g.cleanup()


def phase_usb(instance):
    print(f"\nphase usb: {USB_CROWD} devices on one controller")
    g = Guest(instance, ["--usb", "xhci+crowd"])
    try:
        if not g.start():
            return check("guest boots with the USB crowd", False)
        d = g.console()
        # Hub children enumerate after the hub, one port at a time: wait
        # until the count stops rising rather than for a fixed time.
        count, dump, last_change = -1, "", time.monotonic()
        while time.monotonic() - last_change < 10:
            dump = d.send("usb") or ""
            m = re.search(r"usb: (\d+) device\(s\)", dump)
            n = int(m.group(1)) if m else 0
            if n != count:
                count, last_change = n, time.monotonic()
            time.sleep(1)
        check(f"all {USB_CROWD} devices enumerate", count == USB_CROWD, f"{count} enumerated")
        check("the hub's eight keyboards are among them", dump.count("class 3/1/1") == 8,
              f"{dump.count('class 3/1/1')} keyboards")
        log = d.send("sh dmesg") or ""
        check("a full HID table is reported, not silent", "HID entries in use" in log)
        check("no slot was refused", "out of range" not in log and "enable slot failed" not in log)
        lsusb = d.send("sh lsusb") or ""
        rows = len(re.findall(r"^Port \d+ Device \d+: ID ", lsusb, re.M))
        check("lsusb lists every device", rows == USB_CROWD, f"{rows} rows")
        return True
    finally:
        g.cleanup()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--phase", choices=("all", "pci", "usb"), default="all")
    args = ap.parse_args()

    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("device_count_test: no free VM slot")
            return 2
        args.instance = n
    inst = int(args.instance)
    if not os.path.exists(os.path.join(REPO, "disk.img")):
        print("device_count_test: no disk.img -- run `make iso` first")
        return 2

    if args.phase in ("all", "pci"):
        phase_pci(inst)
    if args.phase in ("all", "usb"):
        phase_usb(inst)

    failed = [n for n, ok, _ in results if not ok]
    print(f"\ndevice_count_test: {len(results) - len(failed)}/{len(results)} checks passed")
    if failed:
        print("device_count_test: FAILED")
        for n in failed:
            print(f"  - {n}")
        return 1
    print("device_count_test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
