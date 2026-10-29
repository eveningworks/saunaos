#!/usr/bin/env python3
"""tools/notify_test.py -- notifications (userland/wm/wm_notify.h,
lib/unotify.h, /bin/notify): the card, the answer going back to the
sender, the bell's history, Do Not Disturb, and each app's own setting.

WHAT THIS ASSERTS THAT "A CARD APPEARED" WOULD NOT
-------------------------------------------------
* THE ANSWER REACHES THE SENDER: `notify --wait` prints the label of the
  action clicked on the card -- a card that only looked clickable passes
  every drawing check and fails this.
* DO NOT DISTURB HOLDS, BUT KEEPS: under it a normal notification puts
  up no card yet is in the history; a critical one shows anyway.
* EACH APP'S SETTING: `off` keeps nothing, `quiet` keeps without a card.
* THE BELL: its badge counts what arrived unseen and opening it clears
  that; a row's x removes it; Clear all empties it; a transient
  notification is never kept.

    python3 tools/vm.py --disk <copy> start
    python3 tools/notify_test.py
    echo $?

POSITIVE CONTROLS: wm_notify_note() returning "show" under Do Not
Disturb reddens the two DND checks; dropping the WIN_EV_NOTIFY push in
wm_notify_answer() reddens "the sender hears which action".
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402

CONF = "/etc/notify.conf"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "notify_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    res = Results()

    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        dbg.send("sh config unset desktop.notifications.dnd")
        dbg.send(f"sh rm {CONF}")

        def nf():
            return dbg.json("gui notify --json")

        def card():
            return dbg.state().get("notice") or {}

        def titles():
            return [r["title"] for r in nf().get("rows", [])]

        def notify(words):
            # tosh -c, unquoted: the kernel shell's spawn keeps quotes.
            dbg.send(f"sh spawn /bin/tosh -c notify {words}")

        # --- a card, its history row, the badge ------------------------
        notify("-a Backup -i drive Copied Three_files")
        c = poll(lambda: (lambda k: k if k.get("title") == "Copied" else None)(card()), 5) or card()
        res.check("a notification puts up its card", c.get("title") == "Copied", f"notice: {c}")
        st = nf()
        res.check("...is kept in the history, unread", "Copied" in titles() and st.get("unread", 0) >= 1,
                  f"notify: {st}")

        # --- the answer reaches the sender -----------------------------
        dbg.send("sh rm /tmp/answer")
        dbg.send("sh spawn /bin/tosh -c notify -a Backup -A Open,Later -w Pick One > /tmp/answer")
        c = poll(lambda: (lambda k: k if k.get("title") == "Pick" else None)(card()), 5) or card()
        btn = next((b for b in c.get("buttons", []) if b["label"] == "Later"), None)
        if btn:
            dbg.click(btn["x"] + btn["w"] // 2, btn["y"] + btn["h"] // 2)
            dbg.settle()
        ans = poll(lambda: "Later" in (dbg.send("sh cat /tmp/answer") or ""), 5)
        res.check("the sender hears which action was clicked (notify --wait prints Later)",
                  bool(btn) and bool(ans), f"button {btn}, answer {dbg.send('sh cat /tmp/answer')!r}")
        res.check("...and an answered notification leaves the history", "Pick" not in titles(), str(titles()))

        # --- Do Not Disturb ----------------------------------------------
        dbg.send("sh config set desktop.notifications.dnd on")
        poll(lambda: nf().get("dnd"), 5)
        dbg.key("0x1b")
        notify("-a Backup Quiet_one")
        poll(lambda: "Quiet_one" in titles(), 5)
        res.check("under Do Not Disturb a normal one shows no card but is kept",
                  card().get("title") != "Quiet_one" and "Quiet_one" in titles(),
                  f"card {card().get('title')!r}, history {titles()}")
        notify("-a Backup -u critical Urgent_one")
        c = poll(lambda: (lambda k: k if k.get("title") == "Urgent_one" else None)(card()), 5) or card()
        res.check("...a critical one shows anyway", c.get("title") == "Urgent_one", f"notice: {c}")
        dbg.send("sh config set desktop.notifications.dnd off")
        poll(lambda: not nf().get("dnd"), 5)

        # --- each app's own setting --------------------------------------
        dbg.send(f"sh config set desktop.notifications.dnd off")
        notify("-a Mailer First")       # writes app.Mailer=on
        poll(lambda: "First" in titles(), 5)
        dbg.send(f"sh write {CONF} app.Mailer=off")
        notify("-a Mailer Hidden_one")
        notify("-a Logger Marker")      # a later notification to wait on
        poll(lambda: "Marker" in titles(), 5)
        res.check("an app turned off keeps nothing and shows nothing", "Hidden_one" not in titles(), str(titles()))
        dbg.send(f"sh write {CONF} app.Mailer=quiet")
        notify("-a Mailer Kept_quietly")
        poll(lambda: "Kept_quietly" in titles(), 5)
        res.check("an app set to quiet is kept without a card",
                  "Kept_quietly" in titles() and card().get("title") != "Kept_quietly",
                  f"card {card().get('title')!r}, history {titles()}")
        notify("-a Mailer -t Passing")
        notify("-a Logger Marker2")
        poll(lambda: "Marker2" in titles(), 5)
        res.check("a transient one is never kept", "Passing" not in titles(), str(titles()))

        # --- the bell ------------------------------------------------------
        st = nf()
        b = st["bell"]
        unread = st.get("unread", 0)
        dbg.click(b[0] + b[2] // 2, b[1] + b[3] // 2)
        st = poll(lambda: (lambda s: s if s.get("open") else None)(nf()), 3) or nf()
        res.check("the bell opens the history and clears the badge",
                  st.get("open") and unread > 0 and st.get("unread") == 0 and st.get("rows"),
                  f"unread before {unread}, notify: {st.get('open')} {st.get('unread')}")
        rows = [r for r in st.get("rows", []) if r["w"]]
        if rows:
            r0 = rows[0]
            n0 = len(st["rows"])
            # Hover first: the x is drawn and hit on the hovered row.
            cx, cy, cs = r0["close"]
            dbg.warp_cursor(qmp, r0["x"] + r0["w"] // 2, r0["y"] + r0["h"] // 2)
            dbg.settle()
            dbg.click(cx + cs // 2, cy + cs // 2)
            after = poll(lambda: (lambda s: s if len(s.get("rows", [])) == n0 - 1 else None)(nf()), 3) or nf()
            res.check("a row's x removes it", len(after.get("rows", [])) == n0 - 1
                      and r0["title"] not in [x["title"] for x in after.get("rows", [])],
                      f"{n0} rows -> {len(after.get('rows', []))}")
        sw = nf()["switch"]
        dbg.click(sw[0] + sw[2] // 2, sw[1] + sw[3] // 2)
        on = poll(lambda: nf().get("dnd") and "on" in (dbg.send("sh config get desktop.notifications.dnd") or ""), 3)
        res.check("the switch turns Do Not Disturb on, as the setting", bool(on),
                  dbg.send("sh config get desktop.notifications.dnd"))
        clr = nf()["clear"]
        dbg.click(clr[0] + clr[2] // 2, clr[1] + clr[3] // 2)
        res.check("Clear all empties the history", poll(lambda: not nf().get("rows"), 3), str(titles()))
        dbg.key("0x1b")
        dbg.send("sh config unset desktop.notifications.dnd")

        # --- Settings > Desktop > Notifications: each app's choice ---------
        # A known list: Backup first, then Mailer.
        dbg.send(f"sh write {CONF} app.Backup=on")
        dbg.send(f"sh append {CONF} app.Mailer=quiet")
        win = dbg.spawn("/bin/wm/system/settings desktop.notifications.dnd", "System Settings")
        w = poll(lambda: (lambda d: d if d.get("nf_mode") else None)(dbg.widgets("System Settings")), 10) or {}
        lst, mode = w.get("nf_apps"), w.get("nf_mode")
        res.check("Settings lists the apps that have notified, with a choice for the one picked",
                  bool(win) and bool(lst) and bool(mode), f"widgets {sorted(w)[:12]}")
        if lst and mode:
            r = lst["screen"]   # its first row: Backup
            dbg.click(r["x"] + 20, r["y"] + 10)
            dbg.settle()
            m = mode["screen"]
            dbg.click(m["x"] + mode["w"] - 10, m["y"] + mode["h"] // 2)   # the last segment, Off
            dbg.settle()
            line = poll(lambda: "app.Backup=off" in (dbg.send(f"sh cat {CONF}") or ""), 5)
            res.check("...and choosing Off writes it for that app at once", bool(line),
                      dbg.send(f"sh cat {CONF}"))
        dbg.send(f"sh rm {CONF}")

    return res.finish("notify_test")


if __name__ == "__main__":
    sys.exit(main())
