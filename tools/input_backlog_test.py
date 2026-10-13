#!/usr/bin/env python3
"""Input a slow client cannot take at once WAITS for it; nothing typed is lost.

WHAT IT CHECKS, and what a broken version would still pass:

  - a burst of 200 characters typed at `/tests/hangclient --slow` (40 ms
    over every key, an app redrawing a heavy page) arrives WHOLE and in
    order -- the client logs what it was typed, compared byte for byte;
  - ...and the compositor's backlog was really in use mid-burst (a
    client that kept up would pass the first check without testing
    anything), and nothing was dropped;
  - a HUNG client (`hangclient`, 'h') whose backlog reaches its cap --
    lowered for the run with `gui evline` -- is marked not responding AT
    ONCE: the ping timeout is raised past the wait, so the mark can only
    come from the backlog, not from a ping.

Shown red by sending input straight to the inbox again (no backlog):
the typed text loses characters.

    python3 tools/input_backlog_test.py [--instance N]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

HANG_BIN = "/tests/hangclient"
TITLE = "Hang Test"
# 200 characters a key name each: letters and digits, no spaces.
TEXT = ("thequickbrownfox0123456789jumpsoverthelazydog" * 5)[:200]

_res = Results()
check = _res.check


def window(dbg):
    for w in dbg.windows():
        if w.get("title") == TITLE:
            return w
    return None


def spawn(dbg, argv, timeout=10.0):
    dbg.logs("hangclient:", clear=True)
    dbg.send(f"gui spawn {argv}")
    deadline = time.time() + timeout
    while time.time() < deadline:
        w = window(dbg)
        if w:
            return w
        time.sleep(0.3)
    return None


def kill(dbg):
    w = window(dbg)
    if w:
        dbg.send(f"sh kill {w['client_pid']}")
        deadline = time.time() + 8
        while window(dbg) and time.time() < deadline:
            time.sleep(0.2)


def slow_client(dbg):
    w = spawn(dbg, f"{HANG_BIN} --slow")
    if not check("the slow client opens", w is not None):
        return
    dbg.settle()
    most = 0
    for i, ch in enumerate(TEXT):
        dbg.send(f"gui key {ch}")
        if i % 40 == 39:               # sample the backlog part-way, without pausing
            w = window(dbg) or {}
            most = max(most, w.get("backlog", 0))
    dbg.send("gui key 0x0d")
    typed = None
    deadline = time.time() + 60
    while time.time() < deadline and typed is None:
        for line in dbg.logs("hangclient: typed", clear=True):
            typed = line.split("hangclient: typed ", 1)[1].strip()
        time.sleep(0.3)
    w = window(dbg) or {}
    check("a 200-key burst at a slow client arrives whole, in order", typed == TEXT,
          f"got {len(typed or '')} chars: {(typed or '')[:60]!r}...")
    check("...the backlog held what the inbox could not, mid-burst", most > 0,
          f"most backlog seen {most}")
    check("...and nothing was dropped", w.get("dropped") == 0 and w.get("backlog") == 0,
          f"dropped {w.get('dropped')} backlog {w.get('backlog')}")
    kill(dbg)


def hung_client(dbg):
    got = dbg.send("gui evline 64") or ""
    if not check("the backlog cap was lowered for the run", "64 events" in got, got.strip()):
        return
    dbg.send("gui pingtimeout 100000")   # the ping cannot be what marks it
    w = spawn(dbg, HANG_BIN)
    if not check("the hang client opens", w is not None):
        return
    dbg.settle()
    dbg.send("gui key h")
    deadline = time.time() + 6
    while time.time() < deadline:
        if any("hanging now" in l for l in dbg.logs("hangclient:", clear=True)):
            break
        time.sleep(0.15)
    for _ in range(300):               # past the 64-slot inbox and the 64-event cap together
        dbg.send("gui key a")
    marked = False
    # Injected keys drain one per compositor frame, so 240 events take
    # seconds to arrive -- still far inside the raised ping timeout.
    deadline = time.time() + 10.0
    while time.time() < deadline and not marked:
        w = window(dbg) or {}
        marked = bool(w.get("not_responding"))
        time.sleep(0.2)
    check("a hung client whose backlog fills is marked not responding at once",
          marked and w.get("dropped", 0) > 0,
          f"not_responding {w.get('not_responding')} dropped {w.get('dropped')}")
    kill(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "input_backlog_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("input_backlog (a slow client loses no input; a hung one is marked)")
    try:
        slow_client(dbg)
        hung_client(dbg)
    finally:
        dbg.send("gui evline 32768")
        dbg.send("gui pingtimeout 300")
    print(f"\ninput_backlog_test: {len(_res.passes)} passed, {len(_res.fails)} failed")
    return 1 if _res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
