#!/usr/bin/env python3
"""The screen off on idle (wm_idle.c, SYS_DISPLAY_POWER), as far as a
guest with nothing to switch can show it.

QEMU's display has no panel sequence and no backlight, so the kernel
REFUSES -- and that refusal is the evidence: the desktop logs it only
after it has really asked. The panel's own power-down and wake are
checked on the Intel laptop by hand (docs/roadmap-details.md).

1. **The timer defaults to 10 minutes** (`gui idle`'s screen_minutes).
2. **Zero means never**: the lever turns nothing off and asks nothing.
3. **One minute, and the lever asks the kernel**, which refuses here:
   `wm: the screen cannot be turned off on this hardware`, and
   `display status` still says on.
"""

RUNNER = {"name": "screenoff", "covers": "the screen-off timer: its default, zero as never, the kernel asked"}

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

REFUSED = "wm: the screen cannot be turned off on this hardware"


def idle(dbg):
    for line in (dbg.send("gui idle") or "").splitlines():
        if line.startswith("{"):
            return json.loads(line)
    return {}


def run(dbg, res):
    st = idle(dbg)
    res.check("the screen-off timer defaults to 10 minutes", st.get("screen_minutes") == 10, str(st))

    dbg.send("sh config set desktop.screen_off 0")
    dbg.logs("wm: ", clear=True)
    said = dbg.send("gui idle screenoff") or ""
    asked = [l for l in dbg.logs("wm: ", clear=True) if "screen" in l]
    res.check("zero means never: the lever turns nothing off and asks nothing",
              "not turned off" in said and not asked, f"{said.strip()} {asked}")

    dbg.send("sh config set desktop.screen_off 1")
    said = dbg.send("gui idle screenoff") or ""
    dbg.settle()
    asked = [l for l in dbg.logs("wm: ", clear=True) if "screen" in l]
    status = dbg.send("sh display status") or ""
    res.check("one minute: the kernel is asked, and refuses on hardware with nothing to switch",
              any(REFUSED in l for l in asked) and "the screen is on" in status,
              f"{said.strip()} {asked} {status.strip()[-60:]}")
    dbg.send("sh config set desktop.screen_off 10")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "screen_off_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, res)
    finally:
        dbg.close()
    print(f"\nscreen_off_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
