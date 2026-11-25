#!/usr/bin/env python3
"""tools/tests/gui/login_test.py -- passwords, sign-out and the login screen.

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/login_test.py

WHAT IT CHECKS (users D2b, docs/users-design.md)
------------------------------------------------
  * System Settings > Users, opened as the desktop's account, shows the
    account, "Set password..." and "Sign in automatically" switched on;
  * its Set password dialog greys Set until the two fields agree, and Set
    stores the password: `passwd -S` says P, and automatic sign-in is
    turned off (`login.autologin` empty);
  * the page then offers Change and Remove, the switch off;
  * the Leave page's Sign out ends the session: the desktop exits and the
    LOGIN SCREEN comes up, run by the greeter, drawn (the field white, the
    arrow in the accent);
  * a wrong password there is refused ("Wrong password"), the field
    emptied, and the greeter stays;
  * the right one starts the session again: a desktop that is not the
    greeter, its compositor running as `user`;
  * (users D2c) behind the login screen the console runs /bin/login, not
    a shell; Super+L locks the session, a wrong password keeps it locked
    and the right one unlocks it.

What a broken version would still pass, and what stops it: a greeter
that let any password in passes the sign-in, so a wrong one must be
refused first; a sign-out that only restarted the desktop passes "a
desktop is back", so the login screen must be seen in between, with
`greeter` true; a Set that saved nothing passes the dialog's closing, so
`passwd -S` is asked.

It puts the machine back: no password, automatic sign-in on.

POSITIVE CONTROL: make privd's pass_check() always 1 -- "a wrong password
is refused" goes red.
"""

RUNNER = {
    "covers": "users D2b: Settings > Users sets a password, Sign out to the login screen, a wrong and a right sign-in",
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

PW = "sauna-lo1"
SETTINGS = "System Settings"
ACCENT = (70, 110, 160)
WM_CURSOR_NORMAL, WM_CURSOR_TEXT = 0, 4   # enum wm_cursor_kind

_res = Results()
check = _res.check


def login_state(dbg):
    """`gui login`, or {} while no compositor answers (one is starting)."""
    try:
        return dbg.json("gui login") or {}
    except (ValueError, OSError):
        return {}


def type_text(dbg, text):
    dbg.input([f"key 0x{ord(c):02x}" for c in text])


def click_widget(dbg, name, title):
    x, y = dbg.widget_center(name, title)
    dbg.click(x, y)


def dialog_widgets(dbg, title, want, timeout=8.0):
    """A uapp DIALOG publishes no widget map, only its layout log
    (`settings.users: layout <name> x y w h`, content-relative, behind
    desktop.layout_log, which enter_gui() turns on): the screen rects."""
    deadline = time.time() + timeout
    found = {}
    while time.time() < deadline and want not in found:
        for ln in dbg.logs("settings.users: layout", clear=True):
            m = re.search(r"layout (\w+) (-?\d+) (-?\d+) (\d+) (\d+)", ln)
            if m:
                found[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        time.sleep(0.2)  # sleep-ok: polls the dialog's layout log, which has no wait of its own
    win = next((w for w in dbg.windows() if w["title"] == title), None)
    if not win:
        return {}
    ox, oy = win["content"]["x"], win["content"]["y"]
    return {n: (ox + r[0] + r[2] // 2, oy + r[1] + r[3] // 2) for n, r in found.items()}


def click_in_dialog(dbg, d, name, title="Set password"):
    """A click, then a SETTLED frame of the dialog: keys typed in the same
    moment as a click on a field were lost, landing in neither field
    (docs/bugs.md) -- with the frame between, every one arrives."""
    if name in d:
        dbg.input([f"click {d[name][0]} {d[name][1]}"])
        dbg.window_settled(title)
    return name in d


def set_password(dbg):
    dbg.as_user("spawn /bin/wm/system/settings login.autologin")
    win = dbg.wait_window(SETTINGS)
    check("System Settings opens, as the desktop's account", win is not None)
    if not win:
        return False
    w = dbg.wait_widget(SETTINGS, "users_set")
    have = dbg.widgets(SETTINGS)
    check("System > Users shows the account, its password and automatic sign-in",
          all(n in have for n in ("users_account", "users_password", "users_auto")) and w is not None,
          ", ".join(sorted(k for k in have if k.startswith("users"))))
    click_widget(dbg, "users_set", SETTINGS)
    dlg = dbg.wait_window("Set password")
    check("Set password... opens its dialog", dlg is not None)
    if not dlg:
        return False
    dbg.window_settled("Set password")
    d = dialog_widgets(dbg, "Set password", "users_ok")
    check("the dialog lays out the new password twice and Set",
          all(n in d for n in ("users_new", "users_ok", "users_cancel")), str(sorted(d)))
    click_in_dialog(dbg, d, "newpw_field")
    type_text(dbg, PW)
    click_in_dialog(dbg, d, "newpw_confirm")
    type_text(dbg, PW[:-1])
    click_in_dialog(dbg, d, "users_ok")
    st = dbg.send("sh passwd -S user") or ""
    check("Set does nothing while the two differ: the dialog stays, no password",
          dbg.window("Set password") is not None and "user NP" in st, st.strip()[-60:])
    click_in_dialog(dbg, d, "newpw_confirm")
    type_text(dbg, PW[-1])
    dbg.key("0x0a")   # Enter in the field is Set, as in any such dialog
    gone = dbg.wait_gone("Set password", timeout=40)
    check("Enter (Set) closes the dialog once privd has the password", gone)
    st = dbg.send("sh passwd -S user") or ""
    check("passwd -S says user has a password", "user P" in st, st.strip()[-60:])
    al = dbg.send("sh config get login.autologin") or ""
    # `config get` prints the value alone: an empty line, no account.
    check("...and automatic sign-in was turned off", al.strip() == "", repr(al.strip()[-80:]))
    dbg.wait_widget(SETTINGS, "users_change")
    have = dbg.widgets(SETTINGS)
    check("the page now offers Change and Remove", "users_change" in have and "users_remove" in have,
          ", ".join(sorted(k for k in have if k.startswith("users"))))
    dbg.close_window(SETTINGS)
    return True


def sign_out(dbg):
    dbg.send("gui leave show signout")
    j = dbg.json("gui leave --json") or {}
    ctl = {c.get("name"): c for c in j.get("controls", [])}
    so = ctl.get("signout")
    check("the Leave page offers Sign out", so is not None, str(sorted(ctl)))
    if not so:
        return False
    dbg.click(so["x"] + so["w"] // 2, so["y"] + so["h"] // 2, settle=False)
    st = poll(lambda: (lambda s: s if s.get("open") and s.get("greeter") else None)(login_state(dbg)), 60, 0.5)
    check("Sign out ends the session: the login screen comes up, run by the greeter", bool(st), str(st)[:160])
    ps = dbg.send("sh ps --user") or ""
    shells = [ln for ln in ps.splitlines() if re.search(r"\btosh\b", ln) and re.search(r"\buser\b", ln)]
    check("...and no shell runs as user behind it (no Ctrl+Alt+F2 past the password)", not shells,
          " | ".join(shells)[:160])
    logins = [ln for ln in ps.splitlines() if re.search(r"\blogin\b", ln) and re.search(r"\broot\b", ln)]
    check("...the console asks instead: /bin/login runs, as root (users D2c)", bool(logins), ps[-200:])
    return bool(st)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=tempfile.gettempdir())
    args = ap.parse_args()
    port_guard.resolve_instance(args, "login_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()
    try:
        if set_password(dbg) and sign_out(dbg):
            st = login_state(dbg)
            ctl = {c["name"]: c for c in st.get("controls", [])}
            scr = dbg.json("gui state --json")["screen"]
            dbg.warp_cursor(qmp, 5, scr["h"] // 2, settle=False)
            shot = os.path.join(args.tmp, "login-screen.png")
            qmp.screenshot(shot)
            img = Image.open(shot).convert("RGB")
            f, go = ctl.get("field"), ctl.get("go")
            if f and go:
                fp = img.getpixel((f["x"] + f["w"] - 6, f["cy"]))
                gp = img.getpixel((go["x"] + 3, go["y"] + 3))
                check("the login screen is drawn: the field white", min(fp) > 230, str(fp))
                check("...and its arrow button in the accent", all(abs(a - b) <= 24 for a, b in zip(gp, ACCENT)),
                      str(gp))
            check("it is for the account `user`", st.get("account") == "user", str(st)[:120])

            lf = {c["name"]: c for c in (st or {}).get("controls", [])}.get("field")
            if lf:
                # THE TEXT CURSOR over the field, the arrow beside it (the
                # Authentication Required dialog shares the overlay path).
                dbg.warp_cursor(qmp, lf["x"] + lf["w"] // 2, lf["y"] + lf["h"] // 2, settle=False)
                on = poll(lambda: dbg.cursor_shape() == WM_CURSOR_TEXT or None, 5, 0.2)
                check("the pointer is the text cursor over the password field", bool(on),
                      f"shape {dbg.cursor_shape()}")
                dbg.warp_cursor(qmp, 5, scr["h"] // 2, settle=False)
                off = poll(lambda: dbg.cursor_shape() == WM_CURSOR_NORMAL or None, 5, 0.2)
                check("...and the arrow off it", bool(off), f"shape {dbg.cursor_shape()}")
                # THE CARET BLINKS: the field differs between frames half a
                # blink apart. A caret drawn solid (nothing woke the WM)
                # gives identical crops in every pair.
                box = (lf["x"], lf["y"], lf["x"] + lf["w"], lf["y"] + lf["h"])
                crops = []
                for i in range(4):
                    pth = os.path.join(args.tmp, f"lock-caret-{i}.png")
                    qmp.screenshot(pth)
                    crops.append(Image.open(pth).convert("RGB").crop(box).tobytes())
                    time.sleep(0.3)  # sleep-ok: sampling a 500 ms blink, the thing asserted
                check("the caret blinks in the field", len(set(crops)) > 1, f"{len(set(crops))} distinct of 4")
            type_text(dbg, "not-it")
            dbg.key("0x0a")
            st = poll(lambda: (lambda s: s if "Wrong password" in s.get("message", "") and not s.get("busy")
                               else None)(login_state(dbg)), 20, 0.5)
            check("a wrong password is refused, and the login screen stays", bool(st) and st.get("greeter"),
                  str(login_state(dbg))[:160])
            check("...its field emptied", bool(st) and st.get("typed") == 0, str(st)[:160])

            type_text(dbg, PW)
            dbg.key("0x0a")
            back = poll(lambda: (lambda s: s if s and not s.get("open") and not s.get("greeter") else None)(
                login_state(dbg)), 60, 0.5)
            check("the right password starts the session: a desktop, not the greeter", bool(back),
                  str(login_state(dbg))[:160])
            ps = dbg.send("sh ps --user") or ""
            row = [ln for ln in ps.splitlines() if re.search(r"\btoywm\b", ln)]
            check("...its compositor running as user", bool(row) and all(re.search(r"\buser\b", r) for r in row),
                  " | ".join(row)[:160])
            sh = poll(lambda: [ln for ln in (dbg.send("sh ps --user") or "").splitlines()
                               if re.search(r"\btosh\b", ln) and re.search(r"\buser\b", ln)], 20, 0.5)
            check("...and the console shell is back, as user", bool(sh), str(sh)[:120])

            # --- the lock screen (users D2c): Super+L, then the password ---
            dbg.key("0x6c", mods="super")
            st = poll(lambda: (lambda s: s if s.get("open") and s.get("mode") == "lock" else None)(
                login_state(dbg)), 10, 0.3)
            check("Super+L locks: the lock screen, in the session (not the greeter)",
                  bool(st) and not st.get("greeter"), str(login_state(dbg))[:160])
            # LOCKED, NO SHELL A VT SWITCH AWAY: the console's and the VTs'
            # shells take their login form while the lock screen is up.
            def locked_ps():
                p = dbg.send("sh ps --user") or ""
                sh = [ln for ln in p.splitlines() if re.search(r"\btosh\b", ln) and re.search(r"\buser\b", ln)]
                lg = [ln for ln in p.splitlines() if re.search(r"\blogin\b", ln) and re.search(r"\broot\b", ln)]
                return (sh, lg) if not sh and lg else None
            got = poll(locked_ps, 15, 0.5)
            check("...and while locked no shell runs as user: the console asks (/bin/login)", bool(got),
                  (dbg.send("sh ps --user") or "")[-240:])
            type_text(dbg, "not-it")
            dbg.key("0x0a")
            st = poll(lambda: (lambda s: s if "Wrong password" in s.get("message", "") and not s.get("busy")
                               else None)(login_state(dbg)), 20, 0.5)
            check("a wrong password keeps it locked", bool(st) and st.get("open"), str(login_state(dbg))[:160])
            type_text(dbg, PW)
            dbg.key("0x0a")
            st = poll(lambda: (lambda s: s if not s.get("open") else None)(login_state(dbg)), 20, 0.5)
            check("the right one unlocks", bool(st), str(login_state(dbg))[:160])
            sh = poll(lambda: [ln for ln in (dbg.send("sh ps --user") or "").splitlines()
                               if re.search(r"\btosh\b", ln) and re.search(r"\buser\b", ln)] or None, 20, 0.5)
            check("...and the console shell is user's again once unlocked", bool(sh), str(sh)[:120])
    finally:
        dbg.send("sh passwd -d user")
        dbg.send("sh config set --machine login.autologin user")
    st = dbg.send("sh passwd -S user") or ""
    check("the machine is put back: no password", "user NP" in st, st.strip()[-60:])
    return _res.finish("login_test")


if __name__ == "__main__":
    sys.exit(main())
