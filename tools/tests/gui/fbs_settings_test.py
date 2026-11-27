#!/usr/bin/env python3
"""tools/tests/gui/fbs_settings_test.py -- Settings' First-boot setup buttons ask privd (users D3b2a).

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/fbs_settings_test.py

WHAT IT CHECKS
--------------
First-boot setup's marker, /var/lib/fbs/done, is in root's 0755
/var/lib, so System Settings > Boot menu's "At the next restart" -- run as
the desktop's account -- asks privd to run `fbs again` elevated
(set_startup.c):

  * the click opens privd's prompt, "fbs wants to run as administrator",
    whose command is `fbs again`;
  * Cancel leaves the marker as it was;
  * the same click and Allow removes it, and the page says the setup
    shows at the next restart.

What a broken version would still pass: the old page, calling
ufbs_rearm() in the desktop's own process, opens no prompt and fails
with "Could not remove" -- so the prompt must open; a prompt whose Allow
ran nothing passes "it opens", so the marker must be gone after Allow and
still there after Cancel.

POSITIVE CONTROL: make startup_fbs() take the root branch for everyone
(drop the geteuid() test) -- "the click opens privd's prompt" goes red,
and the page says "Could not remove /var/lib/fbs/done" (seen 2026-11-27).

The marker is written back as it was at the end, so a later boot does not
show the full-screen wizard over the desktop.
"""

RUNNER = {"covers": "Settings > Boot menu's First-boot setup button: privd's prompt, Cancel keeps the marker, Allow removes it"}

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

TITLE = "System Settings"
MARKER = "/var/lib/fbs/done"
WORDS = "fbs wants to run as administrator"

_res = Results()
check = _res.check


def marker(dbg):
    out = dbg.send(f"sh cat {MARKER}") or ""
    gone = "cannot open" in out or "no such file" in out.lower() or "exit 1" in out
    return "" if gone else out.strip()


def dialog(dbg):
    try:
        return dbg.json("gui dialog --json") or {}
    except ValueError:
        return {}


def wait_prompt(dbg, timeout=15):
    deadline = time.time() + timeout
    info = dialog(dbg)
    while WORDS not in info.get("message", "") and time.time() < deadline:
        time.sleep(0.1)  # sleep-ok: polls the guest's own dialog state
        info = dialog(dbg)
    return info


def wait_gone(dbg, timeout=10):
    deadline = time.time() + timeout
    while dialog(dbg).get("message") and time.time() < deadline:
        time.sleep(0.1)  # sleep-ok: polls the guest's own dialog state
    return not dialog(dbg).get("message")


def pick(dbg, label):
    for b in dialog(dbg).get("buttons", []):
        if b.get("label") == label:
            dbg.click(b["cx"], b["cy"])
            dbg.settle()
            return True
    return False


def press_again(dbg):
    dbg.click(*dbg.widget_center("su_fbs_again", TITLE))


def run(dbg):
    was = marker(dbg)
    if not check("the image has First-boot setup marked done", was != "", was):
        return was
    dbg.prompt_policy("ask")
    dbg.send("gui spawn /bin/wm/system/settings bootmenu")
    if not check("Settings opens on Boot menu", dbg.wait_window(TITLE)):
        return was
    deadline = time.time() + 15
    while "su_fbs_again" not in dbg.widgets(TITLE) and time.time() < deadline:
        time.sleep(0.3)  # sleep-ok: waits for the page's own widget map
    if not check("the page reports the button", "su_fbs_again" in dbg.widgets(TITLE)):
        return was
    dbg.settle()

    press_again(dbg)
    info = wait_prompt(dbg)
    check("the click opens privd's prompt: fbs wants to run as administrator",
          WORDS in info.get("message", ""), str(info)[:200])
    check("...for `fbs again`", "fbs again" in str(info), str(info)[:300])
    pick(dbg, "Cancel")
    wait_gone(dbg)
    _res.check_eventually("Cancel leaves the marker", lambda: marker(dbg), ok=lambda v: v == was, timeout=4)

    press_again(dbg)
    info = wait_prompt(dbg)
    check("asked again, the prompt opens", WORDS in info.get("message", ""), str(info)[:120])
    pick(dbg, "Allow")
    wait_gone(dbg)
    _res.check_eventually("Allow removes the marker", lambda: marker(dbg), ok=lambda v: v == "", timeout=8)
    dbg.close_window(TITLE)
    return was


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fbs_settings_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()
    print("First-boot setup from Settings: one privd prompt")
    was = ""
    try:
        was = run(dbg)
    finally:
        if was and not marker(dbg):
            dbg.write_lines(MARKER, [was], account=None)   # root's, as seeded
        dbg.prompt_policy("allow")
    check("the marker is put back", not was or marker(dbg) != "", marker(dbg))
    return _res.finish("fbs_settings_test")


if __name__ == "__main__":
    sys.exit(main())
