#!/usr/bin/env python3
"""USB on TWO xHCI controllers, each driven as its own bus.

A desktop board has more than one xHCI controller -- a Ryzen has one in
the CPU and one in the chipset -- and the driver used to take the first
and decline the rest, so every port on the others was dead: a Ryzen
3700X on an X470 board lost its keyboard, mouse and USB NIC that way.
`vm.py --usb xhci+two` builds that machine in QEMU: a mouse on the first
controller, and a hub with the keyboard behind it on the SECOND.

THE KEYSTROKE IS THE ORACLE, and it is self-controlling for the reason
usb_test.py gives: with a usb-kbd attached QEMU routes host keys to it
and nowhere else, so if the second controller is not driven nothing
types at all. Behind a hub, so the hub driver's child enumeration is on
the second controller too -- a usb_dev_t cut back to a byte anywhere on
that path names a device on the WRONG controller, and the keyboard
would never bind.

Then the things that only a second controller can get wrong:

  * each controller has its own event ring, so the keyboard types past
    a full ring lap (256 TRBs) and the LAST file must still land;
  * hot-plug runs the second controller's deferred work: the keyboard
    is pulled and plugged back in, and must type again;
  * the names: lsusb's `Bus 1` lines and `devctl`'s `usb:1-` ids, while
    the first controller's devices keep the names they always had.

The oracle is the filesystem, read over the serial console.

    python3 tools/usb_multi_test.py [--instance N] [--kvm]
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402
from usb_test import Guest, type_line, ls_root_until, check, results  # noqa: E402

# Enough keystrokes on the second controller to lap its endpoint's
# 256-TRB ring: each `touch /mNN` is 10 keys, a press and a release
# each, so 14 lines are 280 reports.
WRAP_FILES = 14


def run(instance, kvm):
    g = Guest(instance, "xhci+two", kvm)
    try:
        # Boot 1 sets the text target, so boot 2 has a prompt to type at.
        if not check("guest boots with two xHCI controllers", g.start()):
            return
        d = g.console()
        d.send("sh config set system.default_target text")
        d.send("sh rm /multi1 /multi2" + "".join(f" /m{i:02d}" for i in range(WRAP_FILES)))
        time.sleep(0.4)
        g.stop()
        if not check("guest reboots into the text target", g.start()):
            return
        d = g.console()
        qmp = g.qmp()
        time.sleep(2.0)

        log = d.send("sh dmesg") or ""
        buses = re.findall(r"usb: xHCI [\d.]+ controller .* bus (\d+)", log)
        check("both controllers are brought up, as bus 0 and bus 1",
              buses[:2] == ["0", "1"], f"buses {buses}")
        check("...and neither is declined as a second controller",
              "a second xHCI controller" not in log)
        lsdev = (d.send("lsdev") or "").replace("\t", " ")
        srcs = [ln.strip() for ln in lsdev.splitlines() if "usb-xhci" in ln]
        check("lsdev has a usb-xhci source per controller, each interrupt-driven",
              len(srcs) == 2 and all("[msi" in s or "[irq" in s for s in srcs), " | ".join(srcs))

        lsusb = d.send("sh lsusb") or ""
        bus1 = [ln for ln in lsusb.splitlines() if ln.startswith("Bus 1 Port")]
        check("lsusb lists the hub and the keyboard on Bus 1",
              len(bus1) == 2, " | ".join(bus1) or lsusb[-200:])
        check("...and the first controller's mouse with no bus, as before",
              any(re.match(r"^Port \d+ Device \d+: ID 0627:0001", ln) for ln in lsusb.splitlines()))
        devctl = d.send("sh devctl") or ""
        check("devctl names the second controller's devices usb:1-...",
              "usb:1-1:" in devctl, next((ln.strip() for ln in devctl.splitlines() if "usb:" in ln), ""))

        # The keystroke. Nothing but the second controller can deliver it.
        type_line(qmp, "touch /multi1")
        names = ls_root_until(d, ["multi1"])
        check("a key on the second controller's keyboard reaches the shell (behind its hub)",
              "multi1" in names)

        # A full lap of the keyboard endpoint's ring on that controller.
        want = [f"m{i:02d}" for i in range(WRAP_FILES)]
        for n in want:
            type_line(qmp, f"touch /{n}", delay=0.03)
        names = ls_root_until(d, want, timeout=20.0)
        got = [n for n in want if n in names]
        check(f"the ring laps and the LAST keystroke still lands ({WRAP_FILES} files)",
              len(got) == WRAP_FILES, f"{len(got)}/{WRAP_FILES}")

        # Hot-plug on the second controller: out, then back in.
        qmp.device_del("usbkbd")
        gone = ""
        for _ in range(30):
            gone = d.send("sh lsusb") or ""
            if sum(ln.startswith("Bus 1 Port") for ln in gone.splitlines()) == 1:
                break
            time.sleep(0.5)
        check("pulling the keyboard detaches it from Bus 1",
              sum(ln.startswith("Bus 1 Port") for ln in gone.splitlines()) == 1)
        qmp.device_add("usb-kbd", "usbkbd", bus="xhci2.0", port="1.1")
        back = ""
        for _ in range(40):
            back = d.send("sh lsusb") or ""
            if sum(ln.startswith("Bus 1 Port") for ln in back.splitlines()) == 2:
                break
            time.sleep(0.5)
        check("plugging it back in enumerates it on Bus 1 again",
              sum(ln.startswith("Bus 1 Port") for ln in back.splitlines()) == 2)
        time.sleep(1.0)
        type_line(qmp, "touch /multi2")
        names = ls_root_until(d, ["multi2"])
        check("...and it types again", "multi2" in names)
    finally:
        g.cleanup()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    args = ap.parse_args()
    instance = args.instance if args.instance is not None else port_guard.find_free_instance()
    run(instance, args.kvm)
    failed = [n for n, ok, _ in results if not ok]
    print(f"\nusb_multi_test: {len(results) - len(failed)} passed, {len(failed)} failed")
    for n in failed:
        print("  FAILED:", n)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
