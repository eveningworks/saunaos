#!/usr/bin/env python3
"""tools/tests/gui/standard_signed_test.py -- a Standard account's change, signed by an administrator.

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/standard_signed_test.py

WHAT IT CHECKS (users D3b2b, docs/users-design.md)
--------------------------------------------------
Aino, a STANDARD account, signs in and starts Disks, an administrator
tool. privd no longer refuses her: the prompt asks an administrator to
sign (polkit's auth_admin, KDE's agent's drop-down). With User and Mika
administrators, each with a password:

  * the prompt says an administrator must approve, names Aino as a
    Standard account, and has an Administrator drop-down listing BOTH,
    User chosen, the field asking for User's password;
  * the drop-down is DRAWN (its text's dark pixels inside its box) and
    opens on a click; Mika's row chosen, the field asks for Mika's;
  * User's password typed for Mika is WRONG; Mika's and Allow bring Disks,
    running as root;
  * the next prompt asks again (a signature is never remembered), and
    Cancel keeps Disks away;
  * with no administrator holding a password the prompt only says so: no
    field, one button, Close -- and Disks does not start;
  * `sudo` refuses her before asking anyone.

What a broken version would still pass: a privd that checked the
DEFAULT signer's password whatever was chosen passes "User's password
works", so Mika is chosen and User's password must FAIL for her; a
drop-down listing nobody still opens a dialog, so the count and both
names are asked; an agent that remembered the signature passes the
second prompt's Allow, so the second must show the field again.

POSITIVE CONTROL: make privd's signers_list() return UPRIV_NO_SIGNER --
"the drop-down lists both" goes red; make handle_verdict() check
r->signers' FIRST name instead of the one sent -- "User's password is
wrong for Mika" goes red.
"""

RUNNER = {
    "covers": "users D3b2b: a Standard account's administrator-signed prompt (drop-down, signer's password, no-password refusal, sudo refused)",
}

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402
from PIL import Image                           # noqa: E402

DISKS = "/bin/wm/apps/disks"
PW_USER = "pw-user1"
PW_MIKA = "pw-mika1"

_res = Results()
check = _res.check


def dialog(dbg):
    try:
        return dbg.json("gui dialog --json") or {}
    except ValueError:
        return {}


def buttons(info):
    return {b.get("label"): b for b in info.get("buttons", [])}


def wait_prompt(dbg, timeout=20, words="wants to run as administrator"):
    return poll(lambda: (lambda i: i if words in i.get("message", "") else None)(dialog(dbg)),
                timeout, 0.1) or dialog(dbg)


def disks_windows(dbg):
    w = dbg.json("gui windows --json") or {}
    return [x for x in w.get("windows", []) if x.get("title", "").startswith("Disks")]


def disks_owner(dbg, pid):
    for ln in (dbg.send("sh ps --user") or "").splitlines():
        nums = re.findall(r"\b\d+\b", ln)
        if nums and int(nums[0]) == pid and re.search(r"\bdisks\b", ln):
            return "root" if re.search(r"\broot\b", ln) else ln.split()[1] if len(ln.split()) > 1 else "?"
    return "?"


def type_text(dbg, text):
    dbg.input([f"key 0x{ord(c):02x}" for c in text])


def pick(dbg, label):
    b = buttons(dialog(dbg)).get(label)
    if b:
        dbg.click(b["cx"], b["cy"])
    return b is not None


def set_password(dbg, acct, pw):
    dbg.send(f"sh dash -c \"printf '{pw}\\\\n{pw}\\\\n' | passwd {acct}\"")


def login_state(dbg):
    try:
        return dbg.json("gui login") or {}
    except (ValueError, OSError):
        return {}


def compositor_owner(dbg):
    for ln in (dbg.send("sh ps --user") or "").splitlines():
        if "toywm" in ln:
            m = re.search(r"\b(user|aino|mika|greeter|root)\b", ln)
            if m:
                return m.group(1)
    return ""


def sign_out(dbg):
    dbg.send("gui leave show signout")
    j = dbg.json("gui leave --json") or {}
    so = {c.get("name"): c for c in j.get("controls", [])}.get("signout")
    if not so:
        return False
    dbg.click(so["x"] + so["w"] // 2, so["y"] + so["h"] // 2, settle=False)
    return bool(poll(lambda: (lambda s: s if s.get("open") and s.get("greeter") else None)(login_state(dbg)),
                     60, 0.5))


def sign_in(dbg, name, password=""):
    ctl = poll(lambda: (lambda c: c if c.get("field", {}).get("w") else None)(
        {c["name"]: c for c in login_state(dbg).get("controls", [])}), 20, 0.3) or {}
    row = ctl.get(f"person.{name}")
    if row and row.get("w"):
        dbg.click(row["x"] + row["w"] // 2, row["y"] + row["h"] // 2, settle=False)
        poll(lambda: login_state(dbg).get("account") == name, 10, 0.2)
    if password:
        type_text(dbg, password)
    dbg.key("0x0a", settle=False)
    return poll(lambda: (lambda o: o if o == name else None)(compositor_owner(dbg)), 90, 1.0) or compositor_owner(dbg)


def dark_in(img, x, y, w, h):
    n = 0
    for px in range(x + 4, x + w - 20):   # the arrow at the right left out
        for py in range(y + 3, y + h - 3):
            if sum(img.getpixel((px, py))) < 3 * 110:
                n += 1
    return n


def signed_round(dbg, qmp, tmp):
    dbg.send(f"gui spawn {DISKS}")
    info = wait_prompt(dbg)
    check("Aino's Disks asks for administrator rights instead of being refused",
          "wants to run as administrator" in info.get("message", ""), str(info)[:200])
    check("...saying an administrator must approve it", "An administrator must approve this." in info.get("body", ""),
          info.get("body", ""))
    rows = {k: v for k, v in info.get("rows", [])}
    check("...naming Aino as a Standard account", "aino (" in rows.get("Asked by", "") and
          "Standard account" in rows.get("Asked by", ""), str(rows))
    sg = info.get("signer") or {}
    pw = info.get("password") or {}
    check("the Administrator drop-down lists both administrators, the first chosen",
          sg.get("count") == 2 and sg.get("selected") == 0, str(sg))
    check("...and the field asks for User's password", pw.get("label") == "Password for User", str(pw))
    if not sg or not pw:
        pick(dbg, "Cancel")
        return
    scr = dbg.json("gui state --json")["screen"]
    dbg.warp_cursor(qmp, 5, scr["h"] // 2)
    shot = os.path.join(tmp, "signed-prompt.png")
    qmp.screenshot(shot)
    img = Image.open(shot).convert("RGB")
    ink = dark_in(img, sg["x"], sg["y"], sg["w"], sg["h"])
    check("the drop-down is drawn: its chosen name's text inside the box", ink > 30, f"{ink} dark pixels")
    # Open it, and choose Mika's row (the second) with a click.
    dbg.click(sg["x"] + sg["w"] // 2, sg["y"] + sg["h"] // 2)
    sg = dialog(dbg).get("signer") or {}
    check("a click opens its list", sg.get("open") is True, str(sg))
    dbg.click(sg.get("row_x", 0) + 20, sg.get("row_y", 0) + sg.get("row_h", 0) * 3 // 2)
    info = dialog(dbg)
    sg, pw = info.get("signer") or {}, info.get("password") or {}
    check("...a click on Mika chooses her, and the list closes", sg.get("selected") == 1 and not sg.get("open"),
          str(sg))
    check("...the field now asks for Mika's password", pw.get("label") == "Password for Mika Laine", str(pw))
    type_text(dbg, PW_USER)
    dbg.key("0x0a")
    pw = _res.check_eventually("User's password typed for Mika is wrong",
                               lambda: dialog(dbg).get("password") or {},
                               ok=lambda v: "Wrong password" in v.get("error", "") and not v.get("busy"),
                               timeout=10)
    type_text(dbg, PW_MIKA)
    pick(dbg, "Allow")
    wins = _res.check_eventually("Mika's password and Allow: the Disks window comes",
                                 lambda: disks_windows(dbg), timeout=20)
    if wins:
        check("...running as root", disks_owner(dbg, wins[0].get("client_pid")) == "root",
              str(wins[0].get("client_pid")))
        dbg.close_window("Disks")
        poll(lambda: not disks_windows(dbg), 10, 0.3)


def again_round(dbg):
    dbg.send(f"gui spawn {DISKS}")
    info = wait_prompt(dbg)
    check("the next prompt asks for a password again: a signature is never remembered",
          bool(info.get("password")) and bool(info.get("signer")), str(info)[:200])
    sg = info.get("signer") or {}
    if sg:
        # From the keyboard: the list's arrows move the choice before Enter
        # closes it, and the field must follow.
        dbg.click(sg["x"] + sg["w"] // 2, sg["y"] + sg["h"] // 2)
        dbg.key("0xf781")   # Down
        dbg.key("0x0a")
        info = dialog(dbg)
        sg, pw = info.get("signer") or {}, info.get("password") or {}
        check("Down and Enter in the open list choose Mika, and the field asks for her password",
              sg.get("selected") == 1 and not sg.get("open") and pw.get("label") == "Password for Mika Laine",
              f"{sg} {pw}")
        check("...and Enter there chose, it did not Allow", not pw.get("busy") and not disks_windows(dbg), str(pw))
    pick(dbg, "Cancel")
    time.sleep(3)  # sleep-ok: asserting that NO window comes needs a span to watch
    check("Cancel keeps Disks away", not disks_windows(dbg), str(disks_windows(dbg))[:120])


def no_signer_round(dbg):
    dbg.send("sh passwd -d user")
    dbg.send("sh passwd -d mika")
    dbg.send(f"gui spawn {DISKS}")
    info = wait_prompt(dbg)
    b = buttons(info)
    check("with no administrator password the prompt says why",
          "No administrator on this computer has a password" in info.get("body", ""), info.get("body", ""))
    check("...with no field, no drop-down and one button, Close",
          not info.get("password") and not info.get("signer") and list(b) == ["Close"], str(list(b)))
    pick(dbg, "Close")
    time.sleep(3)  # sleep-ok: asserting that NO window comes needs a span to watch
    check("Close: Disks does not start", not disks_windows(dbg) and not dialog(dbg).get("message"),
          str(disks_windows(dbg))[:120])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=tempfile.gettempdir())
    args = ap.parse_args()
    port_guard.resolve_instance(args, "standard_signed_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()
    for n in ("aino", "mika"):
        dbg.send(f"sh user remove -r {n}")
    out = dbg.send("sh dash -c 'user add -c \"Aino Virtanen\" aino'") or ""
    check("root adds Aino, a Standard account", "added aino" in out, out.strip()[-80:])
    out = dbg.send("sh dash -c 'user add -a -c \"Mika Laine\" mika'") or ""
    check("...and Mika, an administrator", "added mika" in out, out.strip()[-80:])
    set_password(dbg, "user", PW_USER)
    set_password(dbg, "mika", PW_MIKA)
    st = (dbg.send("sh passwd -S user") or "") + (dbg.send("sh passwd -S mika") or "")
    check("both administrators have a password", "user P" in st and "mika P" in st, st.strip()[-120:])
    signed_in = False
    try:
        if not sign_out(dbg):
            check("Sign out brings up the login screen", False)
            return _res.finish("standard_signed_test")
        signed_in = sign_in(dbg, "aino") == "aino"
        check("Aino signs in", signed_in, compositor_owner(dbg))
        if signed_in:
            signed_round(dbg, qmp, args.tmp)
            again_round(dbg)
            out = dbg.send("as aino sudo id") or ""
            check("sudo refuses a Standard account before asking anyone",
                  "aino is not an administrator" in out, out.strip()[-120:])
            no_signer_round(dbg)
    finally:
        dbg.send("sh passwd -d user")
        if signed_in and sign_out(dbg):
            check("...and User signs in again", sign_in(dbg, "user") == "user", compositor_owner(dbg))
        for n in ("aino", "mika"):
            dbg.send(f"sh user remove -r {n}")
        dbg.send("sh rm /var/lib/greeter/last")
    return _res.finish("standard_signed_test")


if __name__ == "__main__":
    sys.exit(main())
