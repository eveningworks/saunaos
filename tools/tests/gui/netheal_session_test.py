#!/usr/bin/env python3
"""tools/tests/gui/netheal_session_test.py -- netheal asks a signed-in person instead of restarting.

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/netheal_session_test.py

WHAT IT CHECKS (2026-11-26, after its reboot met a sign-in on the ASUS)
-----------------------------------------------------------------------
With the network card switched off and its address taken away, and
`system.net.recover` on with a 5 s wait:
  * the kernel counts input since boot (`input.events`; the test's clicks);
  * a person here and signed in (the desktop's account): netheal does NOT restart
    and counts no attempt -- it puts up "No network" with Restart and
    Not now;
  * Not now: nothing happens, netheal says so;
  * the network back while it asks: the question withdrawn;
  * Restart: the desktop's own Leave page runs its Restart (dry run here:
    "leave: would restart"), the apps asked first;
  * the request is ROOT's: netheal run as the desktop's account gets its
    Restart refused by the compositor (WIN_REQ_LEAVE, -EPERM);
  * nobody signed in (the login screen): netheal takes its old path --
    at its two-attempt ceiling, "giving up", so the test needs no reboot.

What a broken version would still pass, and what stops it: a netheal that
still rebooted would end the run, so a file in this boot's /tmp must
still be there; one that never noticed a session passes "no restart" only by
the counter, so the toast must appear; a Restart that ran /bin/reboot
passes "it restarts" -- the Leave page's own line must be in the log; a
compositor that took the request from anyone passes the root case, so a
user's request must be refused.

Puts back: the card up and leased, the settings, the attempt counter,
the Leave page's dry run off, `user` signed in.

POSITIVE CONTROL: make netheal never ask (touched_since_boot() 0) --
it restarts the guest, which ends the run.
Drop wm_client.c's root check -- "a request from a person's program is
refused" goes red.
"""

RUNNER = {
    "covers": "netheal: no restart over a person at the machine, a No network toast with Restart through the Leave page",
}

import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

_res = Results()
check = _res.check


def card(dbg):
    return dbg.state().get("notice") or {}


def wait_card(dbg, title="No network"):
    return poll(lambda: (lambda k: k if k.get("title") == title else None)(card(dbg)), 40)


def press(dbg, c, label):
    b = next((b for b in (c or {}).get("buttons", []) if b["label"] == label), None)
    if b:
        dbg.click(b["x"] + b["w"] // 2, b["y"] + b["h"] // 2)
        dbg.settle()
    return b is not None


def log_has(dbg, needle, timeout=20):
    return poll(lambda: needle in (dbg.send(f"sh dash -c \"dmesg | grep '{needle}'\"") or ""), timeout)


MARK = "/tmp/netheal_test.mark"   # /tmp is ramfs: a restart takes it away


def still_up(dbg):
    return "no such" not in (dbg.send(f"sh stat {MARK}") or "no such").lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=tempfile.gettempdir())
    args = ap.parse_args()
    port_guard.resolve_instance(args, "netheal_session_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()
    m = re.search(r"\b(net-[0-9a-f]+)", dbg.send("sh netctl status") or "")
    dev = m.group(1) if m else ""
    check("the guest has a network card to take down", bool(dev))
    if not dev:
        return _res.finish("netheal_session_test")
    was_on = (dbg.send("sh config get system.net.recover") or "").strip().splitlines()[-1:] or ["off"]
    try:
        dbg.send("sh config set system.net.recover on")
        dbg.send("sh config set system.net.recover.wait 5")
        dbg.send("sh rm /var/lib/netheal")
        dbg.send("gui leave dry on")
        dbg.send(f"sh netctl down {dev}")
        dbg.send(f"sh netctl address {dev} none")

        # --- signed in: a toast, no restart, nothing counted ---
        dbg.send(f"sh dash -c 'echo up > {MARK}'")
        # A PERSON HERE: the pointer moved through QEMU's own mouse device
        # (goto(), relative motion -- `gui warp` sets the kernel's position
        # directly and rightly counts as nobody). An untouched machine is a
        # headless one, and netheal restarts it.
        scr = dbg.json("gui state --json")["screen"]
        qmp.goto(scr["w"] // 2, scr["h"] // 2)
        qmp.goto(scr["w"] // 2 + 40, scr["h"] // 2 + 30)
        dbg.settle()
        out = dbg.send("sh config get input.events") or ""
        m = re.search(r"^\s*(\d+)\s*$", out, re.M)   # the value; a label line follows it
        events = int(m.group(1)) if m else 0
        check("the machine has been touched (the kernel counts input since boot)", events > 0, out.strip()[-80:])
        if not events:
            return _res.finish("netheal_session_test")   # netheal would restart the guest
        dbg.send("sh spawn /bin/netheal")
        c = wait_card(dbg)
        labels = [b["label"] for b in (c or {}).get("buttons", [])]
        check("someone here: no restart, a notification instead -- No network, with Restart and Not now",
              bool(c) and labels == ["Restart", "Not now"], f"card {card(dbg)}")
        cnt = (dbg.send("sh cat /var/lib/netheal") or "").strip()
        check("...and no attempt counted", "no such" in cnt.lower() or cnt.endswith("0") or not cnt, cnt[-60:])
        press(dbg, c, "Not now")
        check("Not now: netheal leaves it", bool(log_has(dbg, "not restarting, as asked")))
        check("...and the machine is still up (this boot's /tmp file is there)", still_up(dbg))

        dbg.send("sh spawn /bin/netheal")
        c = wait_card(dbg)
        press(dbg, c, "Restart")
        check("Restart: the desktop's Leave page runs its Restart, asking the apps first",
              bool(log_has(dbg, "Restart asked by a system program")) and bool(log_has(dbg, "leave: would restart")))

        # --- the request is root's alone ---
        dbg.as_user("spawn /bin/netheal")
        c = wait_card(dbg)
        press(dbg, c, "Restart")
        check("a request from a person's program is refused by the desktop",
              bool(log_has(dbg, "leave: refused a request")) and bool(log_has(dbg, "the desktop refused the restart")))

        # --- the network back while it asks: the question withdrawn ---
        dbg.send("sh spawn /bin/netheal")
        wait_card(dbg)
        dbg.send(f"sh netctl up {dev}")
        dbg.send(f"sh netctl renew {dev}")
        back = log_has(dbg, "the network came back", 40)
        gone = poll(lambda: card(dbg).get("title") != "No network", 20)
        check("the network back while it asks: the question withdrawn", bool(back) and bool(gone),
              f"log {bool(back)}, card {card(dbg).get('title')}")
        dbg.send(f"sh netctl down {dev}")
        dbg.send(f"sh netctl address {dev} none")

        # --- nobody signed in: the old path, at its ceiling ---
        dbg.send("gui leave show signout")
        j = dbg.json("gui leave --json") or {}
        so = {x.get("name"): x for x in j.get("controls", [])}.get("signout")
        if so:
            dbg.send("gui leave dry off")
            dbg.click(so["x"] + so["w"] // 2, so["y"] + so["h"] // 2, settle=False)
            poll(lambda: (dbg.json("gui login") or {}).get("greeter"), 60, 0.5)
        dbg.send("sh dash -c 'echo 2 > /var/lib/netheal'")
        dbg.send("sh spawn /bin/netheal")
        check("nobody signed in: netheal takes its own path (at its ceiling it gives up, no toast)",
              bool(log_has(dbg, "giving up", 30)))
    finally:
        dbg.send(f"sh netctl up {dev}")
        dbg.send(f"sh netctl renew {dev}")
        dbg.send(f"sh config set system.net.recover {was_on[0] if was_on[0] in ('on', 'off') else 'off'}")
        dbg.send("sh config unset system.net.recover.wait")
        dbg.send("sh rm /var/lib/netheal")
        dbg.send("gui leave dry off")
        st = dbg.json("gui login") or {}
        if st.get("open") and st.get("greeter"):
            dbg.key("0x0a", settle=False)   # `user` has no password on a test image
            poll(lambda: not (dbg.json("gui login") or {}).get("greeter"), 60, 0.5)
    return _res.finish("netheal_session_test")


if __name__ == "__main__":
    sys.exit(main())
