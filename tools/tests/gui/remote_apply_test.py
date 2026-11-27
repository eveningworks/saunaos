#!/usr/bin/env python3
"""tools/tests/gui/remote_apply_test.py -- remote desktop is asked for once per Apply (users D3b2a).

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/remote_apply_test.py

WHAT IT CHECKS
--------------
/etc/remote.conf and /etc/remote.secret are root's, so System Settings'
Remote Desktop page STAGES its edits and Apply... sends them to privd as
ONE request (docs/users-design.md, D3b2a):

  * three edits -- the port one up, a trusted address added, a password
    typed -- leave the file alone until Apply;
  * Apply opens ONE prompt, "Settings wants to change remote desktop",
    whose rows name each change -- "Port: 5900 -> 5901", "Trusted:
    + 192.168.77.5", "Password: set" -- and never the password's value;
  * Cancel changes nothing: the file is read back byte for byte;
  * Apply again and Allow makes all three: remote.conf has the port, the
    address and `password_set = yes` but not the password, which is in
    the root-only remote.secret.

What a broken version would still pass: a page that wrote at once (the
old one) passes "Allow makes them", so the file must be unchanged BEFORE
Apply and after Cancel; a prompt per change passes "a prompt opens", so
its rows must list all three; a prompt that showed the password passes
the rest, so no row may carry it; a password left in remote.conf passes
"it is set", so remote.conf must not hold it.

POSITIVE CONTROL: drop the uremote_change_row() loop in privd's words() --
"...whose rows list every change" goes red (seen 2026-11-27). A page that
wrote at once cannot be shown this way any more: as the desktop's account
its write is refused (EACCES), which is the point of the change.
"""

RUNNER = {"covers": "Settings' Remote Desktop page: staged edits, one privd prompt listing them, Cancel and Allow"}

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
ADDR = "192.168.77.5"
PASSWORD = "remote-apply-7"
CONF = "/etc/remote.conf"
SECRET = "/etc/remote.secret"

_res = Results()
check = _res.check


def cat(dbg, path):
    out = dbg.send(f"sh cat {path}") or ""
    return out if "cannot open" not in out and "No such" not in out else ""


def dialog(dbg):
    try:
        return dbg.json("gui dialog --json") or {}
    except ValueError:
        return {}


def wait_prompt(dbg, timeout=15):
    deadline = time.time() + timeout
    info = dialog(dbg)
    while "wants to change remote desktop" not in info.get("message", "") and time.time() < deadline:
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


def typ(dbg, text):
    for ch in text:
        dbg.key(ch, settle=False)
    dbg.settle()


def apply(dbg):
    dbg.click(*dbg.widget_center("apply", TITLE))


def reach(dbg, qmp, name):
    """The control `name`, scrolled into the page's view (the page is taller
    than the window: the trusted-address row starts below it)."""
    for _ in range(12):
        w = dbg.widgets(TITLE)
        c, foot = w[name], w["apply"]
        top = dbg.window(TITLE)["content"]["y"] + 40    # under the page's title
        if c["screen"]["y"] >= top and c["screen"]["y"] + c["h"] <= foot["screen"]["y"] - 8:
            return c
        page = w["rd_when_row"]
        dbg.warp_cursor(qmp, page["screen"]["x"] + 20, foot["screen"]["y"] - 120)
        dbg.wheel(-3 if c["screen"]["y"] > foot["screen"]["y"] - 8 else 3)
    return dbg.widgets(TITLE)[name]


def stage(dbg, qmp):
    port = reach(dbg, qmp, "rd_vnc_port")
    dbg.click(port["screen"]["x"] + port["w"] - 7, port["screen"]["y"] + port["h"] // 4)   # the up arrow
    dbg.settle()
    pw = reach(dbg, qmp, "rd_vnc_pw")
    dbg.click(pw["screen"]["x"] + pw["w"] // 2, pw["screen"]["y"] + pw["h"] // 2)
    typ(dbg, PASSWORD)
    a = reach(dbg, qmp, "rd_add_addr")
    dbg.click(a["screen"]["x"] + a["w"] // 2, a["screen"]["y"] + a["h"] // 2)
    typ(dbg, ADDR)
    b = reach(dbg, qmp, "rd_add")
    dbg.click(b["screen"]["x"] + b["w"] // 2, b["screen"]["y"] + b["h"] // 2)
    dbg.settle()


def run(dbg, qmp):
    before = cat(dbg, CONF)
    secret_before = cat(dbg, SECRET)
    dbg.logs(clear=True)
    dbg.send("gui spawn /bin/wm/system/settings remotedesktop")
    if not check("Settings opens on Remote Desktop", dbg.wait_window(TITLE)):
        return
    deadline = time.time() + 15
    while "rd_vnc_port" not in dbg.widgets(TITLE) and time.time() < deadline:
        time.sleep(0.3)  # sleep-ok: waits for the page's own widget map
    if not check("the page reports its controls", "rd_vnc_port" in dbg.widgets(TITLE)):
        return
    dbg.settle()
    stage(dbg, qmp)
    dbg.settle()
    check("nothing is written before Apply", cat(dbg, CONF) == before, cat(dbg, CONF)[-200:])

    apply(dbg)
    info = wait_prompt(dbg)
    # The dialog's own rows (what asks, and who) are not changes.
    rows = dict((k, v) for k, v in info.get("rows", []) if k not in ("Action", "Program", "Asked by"))
    check("Apply opens ONE prompt: Settings wants to change remote desktop",
          "wants to change remote desktop" in info.get("message", ""), str(info)[:200])
    check("...whose rows list every change", rows.get("Port") == "5900 -> 5901" and
          rows.get("Trusted") == f"+ {ADDR}" and rows.get("Password") in ("set", "changed") and len(rows) == 3,
          str(info.get("rows")))
    check("...and never the password", PASSWORD not in str(info), "")
    pick(dbg, "Cancel")
    wait_gone(dbg)
    _res.check_eventually("Cancel changes nothing", lambda: (cat(dbg, CONF), cat(dbg, SECRET)),
                          ok=lambda v: v == (before, secret_before), timeout=4)

    apply(dbg)                               # the edits stay after a Cancel
    info = wait_prompt(dbg)
    check("asked again, the prompt opens", "wants to change remote desktop" in info.get("message", ""),
          str(info)[:120])
    pick(dbg, "Allow")
    wait_gone(dbg)

    def made():
        c = cat(dbg, CONF)
        return "5901" in c and ADDR in c and "password_set" in c and "yes" in c.split("password_set")[-1][:8]
    _res.check_eventually("Allow makes all three", made, ok=bool, timeout=8)
    now = cat(dbg, CONF)
    check("...remote.conf says a password is set, without it", PASSWORD not in now, now[-200:])
    check("...which is in root's remote.secret", PASSWORD in cat(dbg, SECRET), "")
    said = []
    _res.check_eventually("...and the page says the three were saved",
                          lambda: said.extend(dbg.logs("settings: remote changes")) or said,
                          ok=lambda v: any(ln.rstrip().endswith("changes 3 result 0") for ln in v), timeout=4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "remote_apply_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()
    print("remote desktop: one privd prompt per Apply")
    st = ""
    deadline = time.time() + 10
    while "agent registered" not in st and time.time() < deadline:
        st = dbg.prompt_policy("ask")
        time.sleep(0.2)  # sleep-ok: toywm retries privd every 2 s
    check("the desktop is privd's agent", "ask, agent registered" in st, st)
    conf, secret = cat(dbg, CONF), cat(dbg, SECRET)
    try:
        run(dbg, qmp)
    finally:
        if dialog(dbg).get("message"):
            pick(dbg, "Cancel")
        dbg.close_window(TITLE)
        # Put back what was there: the files as root wrote them.
        dbg.send(f"sh rm {CONF}")
        if conf:
            dbg.write_lines(CONF, conf.splitlines(), account=None)
        dbg.send(f"sh rm {SECRET}")
        if secret:
            dbg.write_lines(SECRET, secret.splitlines(), account=None)
        dbg.prompt_policy("allow")
    return _res.finish("remote_apply_test")


if __name__ == "__main__":
    sys.exit(main())
