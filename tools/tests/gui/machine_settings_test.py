#!/usr/bin/env python3
"""tools/tests/gui/machine_settings_test.py -- the machine's settings ask (users D3b).

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/machine_settings_test.py

WHAT IT CHECKS
--------------
A setting that is neither a person's nor the seat's is root's to change
(docs/users-design.md, D3b): the desktop's account asks privd, and privd
asks the person through the desktop's prompt, listing every change. So:

  * `config set` of one, from a process at the machine (`gui spawn`),
    opens the prompt; it names the change as a row ("Sync every",
    "<old> -> 61 min"); Cancel leaves the value alone, Allow sets it;
  * System Settings stages a machine setting on its page (the spinbox's
    up arrow), Apply opens ONE prompt listing it, and Allow saves it --
    the app says so, and the stored value moved;
  * a person's setting staged with it (none here: the page has one) is not
    in the prompt -- checked from config's side: the desktop's icon size,
    a person's, is set by `as user config` without any prompt.

What a broken version would still pass: a privd that set the value
without asking passes the Allow half, so Cancel must leave it; an agent
that showed the old fixed rows passes "the prompt opens", so the row
naming the change must be there; a Settings that wrote the value itself
(the gate gone) passes "the value moved", so the prompt must have opened
first.

POSITIVE CONTROL: make apply_page() skip the usetting_needs_admin_flags()
branch -- "Apply opens the prompt" goes red (the write is refused, EACCES).
Drop privd's settings rows from words() -- "the prompt names the change"
goes red.
"""

RUNNER = {"covers": "a machine setting asks through privd: config's prompt with its change row, Cancel and Allow; Settings' Apply"}

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

KEY = "system.ntp.interval"     # a declared machine setting, an int in minutes
LABEL = "Sync every"
SETTINGS = "System Settings"
CONTROL_RE = re.compile(r"settings: control (\d+) (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) rows (\d+) kind (\w+)")
APPLY_RE = re.compile(r"settings: layout button apply (-?\d+) (-?\d+) (-?\d+) (-?\d+)")

_res = Results()
check = _res.check


def stored(dbg):
    out = dbg.send(f"sh config get {KEY}") or ""
    m = re.search(r"\b(\d+)\b", out)
    return int(m.group(1)) if m else None


def dialog(dbg):
    try:
        return dbg.json("gui dialog --json") or {}
    except ValueError:
        return {}


def wait_prompt(dbg, timeout=15):
    deadline = time.time() + timeout
    info = dialog(dbg)
    while "change this computer's settings" not in info.get("message", "") and time.time() < deadline:
        time.sleep(0.1)  # sleep-ok: polls the guest's own dialog state
        info = dialog(dbg)
    return info


def pick(dbg, label):
    for b in dialog(dbg).get("buttons", []):
        if b.get("label") == label:
            dbg.click(b["cx"], b["cy"])
            dbg.settle()
            return True
    return False


def row_for(info, key):
    for k, v in info.get("rows", []):
        if k == key:
            return v
    return None


def config_round(dbg, start):
    want = start + 1
    dbg.send(f"gui spawn /bin/config set {KEY} {want}")
    info = wait_prompt(dbg)
    check("config's machine change opens the prompt", "config wants to change this computer's settings"
          in info.get("message", ""), str(info)[:200])
    row = row_for(info, LABEL)
    check("...and names the change as a row", row is not None and f"-> {want}" in row,
          str(info.get("rows"))[:200])
    pick(dbg, "Cancel")
    _res.check_eventually("Cancel leaves the value alone", lambda: stored(dbg), ok=lambda v: v == start, timeout=4)

    dbg.send(f"gui spawn /bin/config set {KEY} {want}")
    info = wait_prompt(dbg)
    check("asked again, the prompt opens", bool(info.get("message")), str(info)[:120])
    pick(dbg, "Allow")
    _res.check_eventually("Allow sets it, as root", lambda: stored(dbg), ok=lambda v: v == want, timeout=6)


def settings_round(dbg, start):
    dbg.logs(clear=True)
    dbg.send(f"gui spawn /bin/wm/system/settings {KEY}")
    if not check("System Settings opens on the page", dbg.wait_window(SETTINGS)):
        return
    ctl = apply = None
    deadline = time.time() + 10
    seen = []
    while time.time() < deadline and (ctl is None or apply is None):
        seen += dbg.logs(clear=True)
        for ln in seen:
            m = CONTROL_RE.search(ln)
            if m and m.group(2).endswith(KEY):
                ctl = tuple(int(m.group(i)) for i in range(3, 7))
            m = APPLY_RE.search(ln)
            if m:
                apply = tuple(int(m.group(i)) for i in range(1, 5))
        time.sleep(0.2)  # sleep-ok: waits for the app's own layout lines
    if not check("the page reports the spinbox and Apply", ctl is not None and apply is not None,
                 f"{ctl} {apply}"):
        return
    win = dbg.window(SETTINGS)
    ox, oy = win["content"]["x"], win["content"]["y"]   # the log's rects are content-relative
    x, y, w, h = ctl
    dbg.click(ox + x + w - 7, oy + y + h // 4)     # the up arrow (ui/uui_spinbox.c)
    dbg.settle()
    ax, ay, aw, ah = apply
    dbg.click(ox + ax + aw // 2, oy + ay + ah // 2)
    info = wait_prompt(dbg)
    check("Apply opens the prompt", "wants to change this computer's settings" in info.get("message", ""),
          str(info)[:200])
    row = row_for(info, LABEL) or ""
    m = re.search(r"-> (\d+)", row)
    want = int(m.group(1)) if m else None
    check("...listing the staged change, one step up", want is not None and want > start, str(info.get("rows"))[:200])
    pick(dbg, "Allow")
    _res.check_eventually("Allow saves it", lambda: stored(dbg), ok=lambda v: v == want, timeout=6)
    said = []
    _res.check_eventually("...and the app says the machine's change was made",
                          lambda: said.extend(dbg.logs("settings: machine changes")) or said,
                          ok=lambda v: any(ln.rstrip().endswith("result 0") for ln in v), timeout=4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "machine_settings_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()
    print("the machine's settings: config and Settings ask through privd")

    st = ""
    deadline = time.time() + 10
    while "agent registered" not in st and time.time() < deadline:
        st = dbg.prompt_policy("ask")
        time.sleep(0.2)  # sleep-ok: toywm retries privd every 2 s
    check("the desktop is privd's agent", "ask, agent registered" in st, st)

    start = stored(dbg)
    check("the setting reads", start is not None, str(start))
    try:
        if start is not None:
            # A person's setting asks nothing: the harness's `as user config`.
            # A declared one -- the kernel's (pointer speed) needs the seat
            # since users D3c, which `as` does not carry.
            icon = (dbg.send("sh config get desktop.icon_size") or "").strip() or "medium"
            out = dbg.as_user(f"config set desktop.icon_size {icon}")
            check("a person's setting is set by the desktop's account, no prompt",
                  "computer's setting" not in (out or "") and not dialog(dbg).get("message"), (out or "")[:120])
            config_round(dbg, start)
            dbg.send(f"sh config --machine set {KEY} {start}")
            settings_round(dbg, start)
    finally:
        if start is not None:
            dbg.send(f"sh config --machine set {KEY} {start}")
        if dialog(dbg).get("message"):
            pick(dbg, "Cancel")
        dbg.close_window(SETTINGS)
        dbg.prompt_policy("deny")
    return _res.finish("machine_settings_test")


if __name__ == "__main__":
    sys.exit(main())
