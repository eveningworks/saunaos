#!/usr/bin/env python3
"""tools/tests/gui/users_page_test.py -- several people: Settings > Users and the login screen.

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/users_page_test.py

WHAT IT CHECKS (users D3a, docs/users-design.md)
------------------------------------------------
  * `user add` (as root) makes an account the Users page then lists under
    Other accounts, with its type drop-down and Remove...;
  * Add account... opens the form (uui_newaccount); a full name typed
    makes the account name, and Add makes the account -- `user` lists it,
    its home is there and the page shows a second row;
  * Remove... on that row, keeping the files, removes the account and
    leaves its home;
  * Sign out ends a windowless program the desktop started (its login
    session's), and leaves one the kernel's shell started (no session);
  * Sign out brings up the login screen with EVERY person bottom-left,
    drawn (a picture's light disc in each row), the one in the middle
    marked; a click on another person puts them in the middle;
  * with two keyboard layouts, the Keyboard button's list opens OVER the
    people rows: the pointer on one of its rows lights no person under it,
    and a click there switches the layout and picks nobody;
  * that person signs in -- a desktop running as them, with the desktop
    launchers their home got from /etc/skel -- and the login screen
    after their sign-out has them in the middle (the greeter remembered).

What a broken version would still pass, and what stops it: a page that
listed nobody passes "Users opens", so the added account's own row must
be there by name; an Add that closed the dialog and made nothing passes
"the dialog closes", so `user` is asked; a list drawn nowhere passes the
greeter's control map, so the row's pixels are read; a click that changed
nothing passes "the screen is up", so `gui login`'s account must change;
a sign-in that started the old account's desktop passes "a desktop is
back", so the compositor's owner is asked.

It puts the machine back: the accounts it made removed with their files,
`user` signed in again.

POSITIVE CONTROL: make wm_login.c's layout() give every person's row
w 0 -- "every person is listed bottom-left" goes red; make privd's
acct_add() return 0 without writing -- "Add makes the account" goes red;
drop init's signal_session() call in session_end() -- "Sign out ends
the session's processes" goes red; drop wm_overlay.c's `i > owner` and its grab -- "the person is not lit"
and "picks nobody" go red.
"""

RUNNER = {
    "covers": "users D3a: Other accounts on Settings > Users, Add/Remove account, the login screen's list of people",
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

SETTINGS = "System Settings"
DISC = (220, 220, 226)   # a picture's light disc (wm_login.c's picture())

_res = Results()
check = _res.check


def login_state(dbg):
    try:
        return dbg.json("gui login") or {}
    except (ValueError, OSError):
        return {}


def controls(dbg):
    return {c["name"]: c for c in login_state(dbg).get("controls", [])}


def type_text(dbg, text):
    dbg.input([f"key 0x{ord(c):02x}" for c in text])


def dialog_widgets(dbg, title, want, timeout=8.0):
    """A uapp dialog's layout log (`settings.users: layout <name> x y w h`,
    content-relative, behind desktop.layout_log) as screen centres."""
    deadline = time.time() + timeout
    found = {}
    while time.time() < deadline and want not in found:
        for ln in dbg.logs("settings.users: layout", clear=True):
            m = re.search(r"layout ([\w.]+) (-?\d+) (-?\d+) (\d+) (\d+)", ln)
            if m:
                found[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        time.sleep(0.2)  # sleep-ok: polls the dialog's layout log, which has no wait of its own
    win = next((w for w in dbg.windows() if w["title"] == title), None)
    if not win:
        return {}
    ox, oy = win["content"]["x"], win["content"]["y"]
    return {n: (ox + r[0] + r[2] // 2, oy + r[1] + r[3] // 2) for n, r in found.items()}


def click_in_dialog(dbg, d, name, title):
    if name in d:
        dbg.input([f"click {d[name][0]} {d[name][1]}"])
        dbg.window_settled(title)
    return name in d


def accounts(dbg):
    out = dbg.send("sh user") or ""
    return {ln.split()[0] for ln in out.splitlines()[1:] if ln.strip() and not ln.startswith("NAME")}


def compositor_owner(dbg):
    ps = dbg.send("sh ps --user") or ""
    for ln in ps.splitlines():
        if "toywm" in ln:
            m = re.search(r"\b(user|anna|aino|greeter|root)\b", ln)
            if m:
                return m.group(1)
    return ""


def settings_round(dbg):
    # From the desktop, as the Start menu starts it: privd asks only the
    # person AT the machine (the seat), which a console `as user` is not.
    dbg.send("gui spawn /bin/wm/system/settings login.autologin")
    if not dbg.wait_window(SETTINGS):
        check("System Settings opens", False)
        return False
    w = dbg.wait_widget(SETTINGS, "users_remove.anna")
    have = dbg.widgets(SETTINGS)
    check("Users lists the added account under Other accounts, with its type and Remove...",
          w is not None and "users_type.anna" in have and "users_others" in have,
          ", ".join(sorted(k for k in have if k.startswith("users"))))
    x, y = dbg.widget_center("users_add", SETTINGS)
    dbg.click(x, y)
    if not dbg.wait_window("Add account"):
        check("Add account... opens its dialog", False)
        return False
    dbg.window_settled("Add account")
    d = dialog_widgets(dbg, "Add account", "users_add_ok")
    check("the dialog lays out the form and Add",
          all(n in d for n in ("newacct_full", "newacct_name", "users_add_ok")), str(sorted(d)))
    click_in_dialog(dbg, d, "newacct_full", "Add account")
    type_text(dbg, "Aino Lahti")
    dbg.key("0x0a")   # Enter in a field is Add
    gone = dbg.wait_gone("Add account", timeout=40)
    check("Enter (Add) closes the dialog once privd made the account", gone)
    have_acct = accounts(dbg)
    st = dbg.send("sh stat /home/aino/Desktop") or ""
    check("Add makes the account: `user` lists aino, its home has the skeleton's Desktop",
          "aino" in have_acct and "No such" not in st, f"{sorted(have_acct)} | {st.strip()[-80:]}")
    w = dbg.wait_widget(SETTINGS, "users_remove.aino")
    check("...and the page shows its row", w is not None)
    if w is None:
        return False
    x, y = dbg.widget_center("users_remove.aino", SETTINGS)
    dbg.click(x, y)
    if not dbg.wait_window("Remove account"):
        check("Remove... opens its dialog", False)
        return False
    dbg.window_settled("Remove account")
    dbg.key("0x0a")   # Enter: Remove, keeping the files (the default)
    gone = dbg.wait_gone("Remove account", timeout=40)
    st = dbg.send("sh stat /home/aino") or ""
    check("Remove, keeping the files: the account is gone, its home stays",
          gone and "aino" not in accounts(dbg) and "No such" not in st, st.strip()[-80:])
    dbg.send("sh dash -c \"rm -r /home/aino\"")
    dbg.close_window(SETTINGS)
    return True


def spawned_pid(out):
    m = re.search(r"as pid (\d+)", out or "")
    return int(m.group(1)) if m else 0


def alive(dbg, pid):
    return any(p["pid"] == pid and p["state"] != "zombie" for p in dbg.processes())


def session_processes_round(dbg):
    """Before Sign out: a windowless program the desktop started (its login
    session's) and one the kernel's shell started (no session). Returns
    their pids, for after_sign_out()."""
    mine = spawned_pid(dbg.send("gui spawn /bin/sleep 900"))
    other = spawned_pid(dbg.send("sh spawn /bin/sleep 901"))
    check("the session starts a windowless program, and the kernel's shell another",
          mine and other and alive(dbg, mine) and alive(dbg, other), f"session {mine}, kernel {other}")
    return mine, other


def after_sign_out(dbg, mine, other):
    if not mine or not other:
        return
    gone = poll(lambda: (not alive(dbg, mine)) or None, 15, 0.5)
    check("Sign out ends the session's processes, not only its windows", bool(gone), f"pid {mine} still there")
    check("...and only the session's: the kernel's shell's program stays", alive(dbg, other), f"pid {other}")
    dbg.send(f"sh kill {other}")


def sign_out(dbg):
    dbg.send("gui leave show signout")
    j = dbg.json("gui leave --json") or {}
    so = {c.get("name"): c for c in j.get("controls", [])}.get("signout")
    if not so:
        check("the Leave page offers Sign out", False)
        return False
    dbg.click(so["x"] + so["w"] // 2, so["y"] + so["h"] // 2, settle=False)
    st = poll(lambda: (lambda s: s if s.get("open") and s.get("greeter") else None)(login_state(dbg)), 60, 0.5)
    return bool(st)


def sign_in(dbg, qmp, name):
    """The person `name` chosen from the list (when there is one), and an
    empty password sent; the new session's compositor's owner."""
    ctl = poll(lambda: (lambda c: c if c.get("field", {}).get("w") else None)(controls(dbg)), 20, 0.3) or {}
    row = ctl.get(f"person.{name}")
    if row and row.get("w"):
        dbg.click(row["x"] + row["w"] // 2, row["y"] + row["h"] // 2, settle=False)
        poll(lambda: login_state(dbg).get("account") == name, 10, 0.2)
    dbg.key("0x0a", settle=False)
    owner = poll(lambda: (lambda o: o if o == name else None)(compositor_owner(dbg)), 90, 1.0)
    return owner or compositor_owner(dbg)


def keyboard_over_people(dbg, qmp):
    """The Keyboard button's list opens over the people rows: the pointer on
    one of its rows lights the row alone, and a click there switches the
    layout and picks nobody (wm_overlay.c's pointer_owner(), the grab)."""
    def hot(name):
        return controls(dbg).get(name, {}).get("hot")

    def open_list():
        kb = poll(lambda: (lambda c: c if c.get("w") else None)(controls(dbg).get("keyboard", {})), 10, 0.3)
        if not kb:
            return None
        dbg.click(kb["cx"], kb["cy"])
        return poll(lambda: (lambda j: j if j.get("open") else None)(dbg.json("gui layout --json") or {}), 5, 0.2)

    def covered(lay):
        """(layout row index, person row) pairs: a list row's centre on a person's row."""
        rows = {n: c for n, c in controls(dbg).items() if n.startswith("person.") and c.get("w")}
        return [(i, n, c) for i, r in enumerate(lay.get("rows", [])) for n, c in rows.items()
                if c["x"] <= r["cx"] < c["x"] + c["w"] and c["y"] <= r["cy"] < c["y"] + c["h"]]

    lay = open_list()
    check("with two layouts the login screen's Keyboard button opens its list", bool(lay),
          str(controls(dbg).get("keyboard")))
    if not lay:
        return
    pairs = covered(lay)
    check("...drawn over a person's row (what this round needs)", bool(pairs), str(lay.get("rows")))
    if not pairs:
        return
    acct = login_state(dbg).get("account")
    # The click below must be able to pick someone NEW: a row over the one in
    # the middle passes "nobody picked" even when broken.
    if all(n == f"person.{acct}" for _, n, _ in pairs):
        scr = dbg.json("gui state --json")["screen"]
        dbg.click(scr["w"] // 2, 8)   # outside the list: it only closes
        other = next((c for n, c in controls(dbg).items() if n.startswith("person.") and n != f"person.{acct}"), None)
        if other:
            dbg.click(other["cx"], other["cy"])
            poll(lambda: login_state(dbg).get("account") != acct, 10, 0.2)
        acct = login_state(dbg).get("account")
        lay = open_list() or {}
        pairs = [p for p in covered(lay) if p[1] != f"person.{acct}"]
    else:
        pairs = [p for p in pairs if p[1] != f"person.{acct}"]
    check("...over someone other than the one in the middle", bool(pairs), f"account {acct}")
    if not pairs:
        return
    # A row that is not the active layout's, so the click changes something.
    pairs.sort(key=lambda p: p[0] == lay.get("active"))
    i, name, person = pairs[0]
    r = lay["rows"][i]
    # The row lit with the list shut first, so "not lit" below cannot pass
    # on a pointer that never arrived or a `hot` nobody reports.
    dbg.key(0x1b)
    poll(lambda: not (dbg.json("gui layout --json") or {}).get("open"), 5, 0.2)
    dbg.warp_cursor(qmp, person["cx"], person["cy"])
    check("the pointer on a person's row lights it", bool(poll(lambda: hot(name) or None, 5, 0.2)),
          str(controls(dbg).get(name)))
    lay = open_list() or {}
    r = (lay.get("rows") or [r])[i]
    dbg.warp_cursor(qmp, r["cx"], r["cy"])
    dbg.settle()
    check("...but on the Keyboard list over it, the person is not lit", hot(name) is False,
          str(controls(dbg).get(name)))
    before = (dbg.send("sh config get system.keyboard.layout") or "").strip()
    dbg.click(r["cx"], r["cy"])
    v = _res.check_eventually("a click on the list's row switches the layout",
                              lambda: (dbg.send("sh config get system.keyboard.layout") or "").strip(),
                              ok=lambda v: v and v != before, timeout=5)
    dbg.settle()
    check("...and picks nobody: the one in the middle stays", login_state(dbg).get("account") == acct,
          f"{login_state(dbg).get('account')} (was {acct}, row over {name})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=tempfile.gettempdir())
    args = ap.parse_args()
    port_guard.resolve_instance(args, "users_page_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    dbg.settle()
    for n in ("anna", "aino"):
        dbg.send(f"sh user remove -r {n}")
    # Through dash: the console's own `sh` does not group a quoted word.
    out = dbg.send("sh dash -c 'user add -c \"Anna Virtanen\" anna'") or ""
    check("root adds an account with `user add`", "added anna" in out, out.strip()[-80:])
    try:
        if not settings_round(dbg):
            check("Sign out brings up the login screen", False)
            return _res.finish("users_page_test")
        mine, other = session_processes_round(dbg)
        if not sign_out(dbg):
            check("Sign out brings up the login screen", False)
            return _res.finish("users_page_test")
        after_sign_out(dbg, mine, other)
        ctl = poll(lambda: (lambda c: c if c.get("person.anna", {}).get("w") else None)(controls(dbg)), 20, 0.3) or {}
        rows = {n: c for n, c in ctl.items() if n.startswith("person.")}
        check("the login screen lists every person bottom-left", set(rows) >= {"person.user", "person.anna"} and
              all(c.get("w") for c in rows.values()), str({n: c.get("w") for n, c in rows.items()}))
        check("...the one who signed in last in the middle", login_state(dbg).get("account") == "user",
              str(login_state(dbg).get("account")))
        scr = dbg.json("gui state --json")["screen"]
        dbg.warp_cursor(qmp, scr["w"] - 5, 5, settle=False)
        shot = os.path.join(args.tmp, "users-login.png")
        qmp.screenshot(shot)
        img = Image.open(shot).convert("RGB")
        drawn = 0
        for n, c in rows.items():
            # The picture's disc at the row's left (wm_login.c: radius
            # h/2 - u()/2 - 2, centre u()/2 + 2 + r in, at scale 1), sampled
            # left of the head, above the shoulders -- the plain disc.
            r = c["h"] // 2 - 4 - 2
            cx, cy = c["x"] + 4 + 2 + r, c["y"] + c["h"] // 2
            px = img.getpixel((cx - r * 6 // 10, cy - r // 4))
            drawn += all(abs(a - b) <= 12 for a, b in zip(px, DISC))
        check("...each row drawn: its picture's light disc", len(rows) >= 2 and drawn == len(rows),
              f"{drawn} of {len(rows)}")
        layouts = (dbg.send("sh config get system.keyboard.layouts") or "").strip()
        active = (dbg.send("sh config get system.keyboard.layout") or "").strip()
        try:
            dbg.send("sh config --machine set system.keyboard.layouts fi,us")
            dbg.send("sh config --machine set system.keyboard.layout fi")
            keyboard_over_people(dbg, qmp)
        finally:
            dbg.send(f"sh config --machine set system.keyboard.layouts {layouts or 'fi'}")
            dbg.send(f"sh config --machine set system.keyboard.layout {active or 'fi'}")
        owner = sign_in(dbg, qmp, "anna")
        check("a click on Anna, then Enter: her desktop starts, its compositor running as anna",
              owner == "anna", owner)
        st = dbg.send("sh stat /home/anna/Desktop/files.desktop") or ""
        check("...with the launchers her home got from /etc/skel", "No such" not in st, st.strip()[-80:])
        # fsperm's refusals (kernel/fs/fsperm.c) for her uid, the one sign
        # a program of hers tried a file the machine keeps for `user`.
        uid = (dbg.send("sh id -u anna") or "").strip().splitlines()[-1:] or ["?"]
        out = dbg.send("sh dash -c \"dmesg | grep fsperm\"") or ""
        refusals = [ln for ln in out.splitlines() if f"uid {uid[0]} refused" in ln]
        check("...and nothing she started was refused a file it reads", not refusals, " | ".join(refusals)[:300])
        if sign_out(dbg):
            got = poll(lambda: (lambda a: a if a == "anna" else None)(login_state(dbg).get("account")), 10, 0.3)
            check("after her sign-out the login screen has Anna in the middle (who signed in last)",
                  got == "anna", str(login_state(dbg).get("account")))
        owner = sign_in(dbg, qmp, "user")
        check("...and User signs in again", owner == "user", owner)
    finally:
        dbg.send("sh user remove -r anna")
        dbg.send("sh dash -c \"rm -rf /home/aino\"")
        dbg.send("sh rm /var/lib/greeter/last")
    return _res.finish("users_page_test")


if __name__ == "__main__":
    sys.exit(main())
