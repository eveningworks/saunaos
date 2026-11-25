#!/usr/bin/env python3
"""tools/tests/gui/privd_prompt_test.py -- the Authentication Required prompt.

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/privd_prompt_test.py

WHAT IT CHECKS
--------------
Disks is an administrator tool (base/upriv.h, upriv_elevate_self()):
started as `user`, it asks privd to run it elevated, and privd asks the
person through the desktop (toywm's wm_priv.c). So:

  * the desktop registered as privd's agent;
  * the prompt opens, names Disks, and offers Allow and Cancel;
  * it is DRAWN -- the Allow button is filled with the accent and the
    Cancel button is not (pixel values, the pointer parked away);
  * Cancel: no Disks window comes, and its process is gone;
  * Allow: the Disks window comes, and the process behind it is ROOT;
  * WITH A PASSWORD (users D2a): the prompt asks for user's, a wrong one
    typed and Entered comes back as "Wrong password" with the field
    emptied, its dots drawn and then gone; the right one and Allow
    bring Disks, as root.

What a broken version would still pass: an agent that answered Allow by
itself passes the Allow half, so Cancel must keep Disks away; a privd
that ran Disks without asking passes both window checks, so the prompt
must have been open first; a Disks that never elevated still opens its
window, so its owner must be root; a field that took no keys passes the
"emptied" check, so the typed count must reach the length first.

POSITIVE CONTROL: make wm_priv_poll() answer every question with 1
before showing it -- "the prompt opens" and "Cancel keeps Disks away"
go red. Make privd's pass_check() always 1 -- "a wrong password comes
back" goes red.
"""

RUNNER = {"covers": "privd's consent prompt, Cancel and Allow, by pixel and by owner"}

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402
from PIL import Image  # noqa: E402

DISKS = "/bin/wm/apps/disks"
ACCENT = (70, 110, 160)
PW = "pw-gui1"

_res = Results()
check = _res.check


def dialog(dbg):
    try:
        return dbg.json("gui dialog --json") or {}
    except ValueError:
        return {}


def button(info, label):
    for b in info.get("buttons", []):
        if b.get("label") == label:
            return b
    return None


def disks_windows(dbg):
    w = dbg.json("gui windows --json") or {}
    return [x for x in w.get("windows", []) if x.get("title", "").startswith("Disks")]


def disks_procs(dbg):
    """(pid, user) for each live disks process, from `ps --user`."""
    out = dbg.send("sh ps --user") or ""
    rows = []
    for ln in out.splitlines():
        if re.search(r"\bdisks\b", ln):
            nums = re.findall(r"\b\d+\b", ln)
            user = "root" if re.search(r"\broot\b", ln) else ("user" if re.search(r"\buser\b", ln) else "?")
            if nums:
                rows.append((int(nums[0]), user))
    return rows


def wait_prompt(dbg, timeout=15):
    deadline = time.time() + timeout
    info = dialog(dbg)
    while "wants to run as administrator" not in info.get("message", "") and time.time() < deadline:
        time.sleep(0.1)  # sleep-ok: polls the guest's own dialog state
        info = dialog(dbg)
    return info


def type_text(dbg, text):
    dbg.input([f"key 0x{ord(c):02x}" for c in text])


def dark_in(img, r):
    """Pixels darker than mid-grey inside the field, away from its frame."""
    n = 0
    for x in range(r["x"] + 4, r["x"] + r["w"] - 4):
        for y in range(r["y"] + 4, r["y"] + r["h"] - 4):
            if sum(img.getpixel((x, y))) < 3 * 110:
                n += 1
    return n


def password_round(dbg, qmp, tmp):
    # dash for printf, which the shell has no builtin for.
    dbg.send(f"sh dash -c \"printf '{PW}\\\\n{PW}\\\\n' | passwd user\"")
    st = dbg.send("sh passwd -S user") or ""
    check("passwd sets user's password from a pipe", "user P" in st, st.strip()[-80:])
    dbg.send(f"gui spawn {DISKS}")
    info = wait_prompt(dbg)
    pw = info.get("password") or {}
    check("the prompt now asks for user's password", pw.get("label") == "Password for User", str(info)[:200])
    if not pw:
        return
    type_text(dbg, "wrong")
    pw = dialog(dbg).get("password") or {}
    check("...and the field takes the keys", pw.get("typed") == 5, str(pw))
    scr = dbg.json("gui state --json")["screen"]
    dbg.warp_cursor(qmp, 5, scr["h"] // 2)
    shot = os.path.join(tmp, "privd-prompt-pw.png")
    qmp.screenshot(shot)
    typed_dark = dark_in(Image.open(shot).convert("RGB"), pw)
    check("...drawing them masked, as dots", typed_dark > 10, f"{typed_dark} dark pixels")
    dbg.key("0x0a")   # Enter
    pw = _res.check_eventually("a wrong password comes back as Wrong password, the field emptied",
                               lambda: dialog(dbg).get("password") or {},
                               ok=lambda v: "Wrong password" in v.get("error", "") and not v.get("busy")
                               and v.get("typed") == 0, timeout=10)
    if pw:
        qmp.screenshot(shot)
        left = dark_in(Image.open(shot).convert("RGB"), pw)
        # The caret stays (a 2-pixel bar); five dots are what must go.
        check("...with its dots gone, the caret alone left", left + 20 <= typed_dark,
              f"{left} dark pixels now, {typed_dark} with five typed")
    type_text(dbg, PW)
    pick(dbg, "Allow")
    wins = _res.check_eventually("the right password and Allow: the Disks window comes",
                                 lambda: disks_windows(dbg), timeout=20)
    if wins:
        owner = wins[0].get("client_pid")
        procs = dict(disks_procs(dbg))
        check("...as root", procs.get(owner) == "root", f"pid {owner}, ps {procs}")
        dbg.close_window("Disks")


def pick(dbg, label):
    b = button(dialog(dbg), label)
    if b:
        dbg.click(b["cx"], b["cy"])
    return b is not None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=tempfile.gettempdir())
    args = ap.parse_args()
    port_guard.resolve_instance(args, "privd_prompt_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()

    print("privd's prompt: Cancel, then Allow")
    st = ""
    deadline = time.time() + 10
    while "agent registered" not in st and time.time() < deadline:
        st = dbg.prompt_policy("ask")
        time.sleep(0.2)  # sleep-ok: toywm retries privd every 2 s
    check("the desktop is privd's agent, and shows its questions", "ask, agent registered" in st, st)

    # --- Cancel ---
    dbg.send(f"gui spawn {DISKS}")
    info = wait_prompt(dbg)
    check("the prompt opens and names Disks", "Disks wants to run as administrator" in info.get("message", ""),
          str(info)[:200])
    allow, cancel = button(info, "Allow"), button(info, "Cancel")
    check("...with Allow and Cancel", allow is not None and cancel is not None, str(info.get("buttons"))[:200])
    if allow and cancel:
        scr = dbg.json("gui state --json")["screen"]
        dbg.warp_cursor(qmp, 5, scr["h"] // 2)   # parked off both buttons
        dbg.settle()
        shot = os.path.join(args.tmp, "privd-prompt.png")
        qmp.screenshot(shot)
        img = Image.open(shot).convert("RGB")
        a_px = img.getpixel((allow["x"] + 3, allow["cy"]))
        c_px = img.getpixel((cancel["x"] + 3, cancel["cy"]))
        check("Allow is drawn in the accent", all(abs(a - b) <= 24 for a, b in zip(a_px, ACCENT)), str(a_px))
        check("...and Cancel is not", any(abs(a - b) > 40 for a, b in zip(c_px, ACCENT)), str(c_px))
        pick(dbg, "Cancel")
    check("Cancel closes the prompt", "message" not in dialog(dbg) or not dialog(dbg).get("message"),
          str(dialog(dbg))[:120])
    _res.check_eventually("...and no Disks window comes, nor a disks process", lambda: (disks_windows(dbg), disks_procs(dbg)),
                          ok=lambda v: not v[0] and not v[1], timeout=4)

    # --- Allow ---
    dbg.send(f"gui spawn {DISKS}")
    info = wait_prompt(dbg)
    check("the prompt opens again", "Disks wants to run as administrator" in info.get("message", ""),
          str(info)[:200])
    pick(dbg, "Allow")
    wins = _res.check_eventually("Allow: the Disks window comes", lambda: disks_windows(dbg), timeout=15)
    if wins:
        owner = wins[0].get("client_pid")
        procs = dict(disks_procs(dbg))
        check("...and the process behind it is root", procs.get(owner) == "root", f"pid {owner}, ps {procs}")
        dbg.close_window("Disks")

    # --- with a password ---
    try:
        password_round(dbg, qmp, args.tmp)
    finally:
        dbg.send("sh passwd -d user")   # root: no current password asked
    st = dbg.send("sh passwd -S user") or ""
    check("passwd -d removes it again", "user NP" in st, st.strip()[-80:])
    dbg.prompt_policy("allow")
    return _res.finish("privd_prompt_test")


if __name__ == "__main__":
    sys.exit(main())
