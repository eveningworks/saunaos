#!/usr/bin/env python3
"""The Software app: its cards, the queue and Apply -- a component removed and added back -- the list, the GPL switch.

docs/software-design.md, stage 5. Geometry is the app's own report
(`software: layout cards.button i ...`, `software: card i <id> <state>`),
the outcome is read by INDEPENDENT readers (`ls`, `sum`, `config get`),
and what a broken version would still pass is named beside each check:

  - it opens on CARDS, one per staged component (a view listing nothing,
    or every component twice, fails the count), with ink in a card;
  - Remove on DOOM's card QUEUES it -- the card says so and the footer's
    Discard and Apply appear -- and nothing is removed until Apply
    (DOOM's program is still there); Discard drops it;
  - Remove on the Web page engine queues the Web Browser too (Needs=);
  - Remove Demos and tests, Apply, the confirm's Apply: `ls` no longer
    finds /bin/wm/demos/counter and the card offers Install; the
    sidebar's Available count is 1;
  - Install it, Apply: fetched at this machine's build from the server
    here, `sum` agrees with the manifest, the card says installed;
  - View > List (Ctrl+2): the rows and the details pane, its Remove
    button, and ink in the pane;
  - the switch: off writes software.show_gpl=off (read with `config`),
    the GPL components' cards go and the banner's "Show them" appears;
    "Show them" turns it back on.

The server is tools/dev/update_server.py's handler in this process, on an
ephemeral loopback port the guest reaches as 10.0.2.2: nothing leaves
this machine.

    python3 tools/tests/gui/software_test.py --instance 8
    python3 tools/tests/gui/software_test.py --instance 8 --positive-control

THE POSITIVE CONTROL serves no per-build route, as a server that pruned
the guest's build: the Install must then fail ("update first") and the
"sum agrees" check must go red.
"""

RUNNER = {"covers": "the Software app: cards, the queue, Apply (remove and add back), the list, the GPL switch"}

import argparse
import http.server
import os
import re
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
import update_server                            # noqa: E402
from harness import Results, poll               # noqa: E402

APP, TITLE = "/bin/wm/system/software", "Software"
ROOT = toolpath.REPO
STAGING = os.path.join(ROOT, "seed", "sync")


def serve(no_builds):
    manifest = update_server.Manifest(STAGING)
    base = update_server.make_handler({"": manifest})

    class Quiet(base):
        def _say(self, *a):
            pass

        def do_GET(self):
            if no_builds and "/builds/" in self.path:
                self._send(404, b"build not on this server\n")
                return
            super().do_GET()

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, manifest


class App:
    """The app's own reports, read from its layout log."""

    def __init__(self, dbg):
        self.dbg = dbg

    def lines(self):
        return self.dbg.logs("software:", clear=False)

    def state(self):
        for ln in reversed(self.lines()):
            m = re.search(r"software: state view (\w+) cat (\d+) cards (\d+) queue (\d+) gpl (\d) "
                          r"counts (\d+) (\d+) (\d+)", ln)
            if m:
                return {"view": m.group(1), "cards": int(m.group(3)), "queue": int(m.group(4)),
                        "gpl": int(m.group(5)), "all": int(m.group(6)), "installed": int(m.group(7)),
                        "available": int(m.group(8))}
        return {}

    def cards(self):
        """{id: (index, state)} for the latest frame's cards."""
        out, n = {}, None
        for ln in reversed(self.lines()):
            m = re.search(r"software: card (\d+) (\S+) (\S+)", ln)
            if m:
                i = int(m.group(1))
                if n is not None and i >= n:
                    break           # an earlier frame's list
                n = i
                out.setdefault(m.group(2), (i, m.group(3)))
        return out

    def rect(self, part, i=None):
        pat = rf"software: layout {re.escape(part)} " + (rf"{i} " if i is not None else "") + \
              r"(-?\d+) (-?\d+) (\d+) (\d+)$"
        for ln in reversed(self.lines()):
            m = re.search(pat, ln)
            if m:
                return tuple(int(v) for v in m.groups())
        return None

    def wait(self, ok, timeout=15):
        return poll(lambda: (lambda s: s if ok(s) else None)(self.state()), timeout, 0.3)


def click_rect(dbg, win, r):
    c = win["content"]
    dbg.click(c["x"] + r[0] + r[2] // 2, c["y"] + r[1] + r[3] // 2)
    dbg.settle()


def dialog_button(dbg, win, app, not_at=None, timeout=15):
    """Click the open dialog's first button -- Apply, or Close -- where the
    dialog itself reports it (disks_test.py's way). `not_at`: the rect the
    last dialog's button had, so a stale one is never clicked. The rect."""
    r = poll(lambda: (lambda r: r if r and r != not_at else None)(app.rect("dialog.button", 0)), timeout, 0.3)
    if r:
        click_rect(dbg, win, r)
    return r


def ink(qmp, tmp, x, y, w, h):
    from PIL import Image
    p = os.path.join(tmp, f"software_{os.getpid()}.png")
    qmp.screenshot(p)
    im = Image.open(p).convert("RGB").crop((x, y, x + w, y + h))
    return sum(1 for px in im.getdata() if sum(px) < 300)


def summed(dbg, path):
    for line in dbg.send(f"sh sum {path}").splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == path and f[0].isdigit():
            return int(f[0]), int(f[1])
    return None


def crc_of(manifest, path):
    text, _ = manifest.build()
    for line in text.splitlines():
        f = line.split(" ")
        if len(f) >= 3 and f[2] == path:
            return int(f[0])
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--positive-control", action="store_true",
                    help="serve no per-build route: the Install must fail")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "software_test")
    res = Results()
    why = update_server._stale_reason(STAGING)
    if why:
        res.check("the staging tree matches the build", False, f"{why} -- run `make iso`")
        return res.finish("software_test")
    staged = sorted(n[:-10] for n in os.listdir(os.path.join(STAGING, "usr/share/components"))
                    if n.endswith(".component"))
    srv, manifest = serve(args.positive_control)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    app = App(dbg)
    print("the Software app (userland/apps/system/software/, ui/uui_appcard)")
    try:
        dbg.send(f"sh update --server http://10.0.2.2:{srv.server_address[1]}")
        dbg.send("sh config set software.show_gpl on")
        if dbg.window(TITLE):
            dbg.close_window(TITLE)
        dbg.logs("software:", clear=True)
        win = dbg.spawn(APP, TITLE)
        st = app.wait(lambda s: s.get("cards") == len(staged))
        res.check("it opens on cards, one per staged component",
                  win is not None and st and st["view"] == "cards" and sorted(app.cards()) == staged,
                  f"{st} {sorted(app.cards())} vs {staged}")
        c = win["content"] if win else {"x": 0, "y": 0}
        r0 = app.rect("cards.item", 0)
        res.check("...and a card is drawn", r0 is not None and
                  ink(qmp, args.tmp, c["x"] + r0[0], c["y"] + r0[1], r0[2], r0[3]) > 200)
        res.check("the sidebar counts every one installed, none available",
                  st and st["installed"] == len(staged) and st["available"] == 0, str(st))

        def press(cid):
            i = app.cards().get(cid, (None,))[0]
            r = app.rect("cards.button", i) if i is not None else None
            if r:
                click_rect(dbg, win, r)
            return r is not None

        # The queue: DOOM's Remove marks it, Discard drops it.
        res.check("DOOM has a card with a button", press("doom"))
        st = app.wait(lambda s: s.get("queue") == 1)
        res.check("Remove queues it: the card says so", st and app.cards().get("doom", (0, ""))[1] == "remove",
                  f"{st} {app.cards().get('doom')}")
        w = dbg.widgets(TITLE)
        res.check("...the footer's Discard and Apply appear", "discard" in w and "apply" in w, str(sorted(w)))
        res.check("...and nothing is removed yet", "doom" in dbg.send("sh ls /bin/wm/apps").split())
        if "discard" in w:
            click_rect(dbg, win, (w["discard"]["x"], w["discard"]["y"], w["discard"]["w"], w["discard"]["h"]))
        st = app.wait(lambda s: s.get("queue") == 0)
        res.check("Discard drops it", st and app.cards().get("doom", (0, ""))[1] == "installed", str(st))

        # Needs=: the engine takes the Browser with it.
        press("webengine")
        st = app.wait(lambda s: s.get("queue") == 2)
        res.check("removing the Web page engine queues the Web Browser too",
                  st and app.cards().get("browser", (0, ""))[1] == "remove", f"{st} {app.cards()}")
        w = dbg.widgets(TITLE)
        if "discard" in w:
            click_rect(dbg, win, (w["discard"]["x"], w["discard"]["y"], w["discard"]["w"], w["discard"]["h"]))
        app.wait(lambda s: s.get("queue") == 0)

        # Remove Demos and tests, for real.
        press("demos")
        app.wait(lambda s: s.get("queue") == 1)
        w = dbg.widgets(TITLE)
        dbg.logs("software:", clear=True)
        if "apply" in w:
            click_rect(dbg, win, (w["apply"]["x"], w["apply"]["y"], w["apply"]["w"], w["apply"]["h"]))
        asked = poll(lambda: any("software: ask apply 1" in ln for ln in app.lines()), 10, 0.3)
        res.check("Apply asks first, with the steps", asked)
        at = dialog_button(dbg, win, app)   # Apply: its rect came with the ask, so the log is not cleared
        done = poll(lambda: next((ln for ln in app.lines() if "software: apply done" in ln), None), 120, 0.5)
        res.check("the removal runs", done and "apply done 1 of 1" in done, str(done))
        res.check("...and /bin/wm/demos/counter is gone, by ls",
                  "counter" not in dbg.send("sh ls /bin/wm/demos").split())
        dialog_button(dbg, win, app, at)    # Close
        st = app.wait(lambda s: s.get("available") == 1)
        res.check("...the card offers Install, the sidebar counts it available",
                  st and app.cards().get("demos", (0, ""))[1] == "available", f"{st} {app.cards().get('demos')}")

        # Install it back, from the server, at this build.
        press("demos")
        app.wait(lambda s: s.get("queue") == 1)
        w = dbg.widgets(TITLE)
        dbg.logs("software:", clear=True)
        if "apply" in w:
            click_rect(dbg, win, (w["apply"]["x"], w["apply"]["y"], w["apply"]["w"], w["apply"]["h"]))
        poll(lambda: any("software: ask apply 1" in ln for ln in app.lines()), 10, 0.3)
        at = dialog_button(dbg, win, app)   # Apply
        done = poll(lambda: next((ln for ln in app.lines() if "software: apply done" in ln), None), 300, 0.5)
        res.check("the install runs", done and "apply done 1 of 1" in done, str(done))
        dialog_button(dbg, win, app, at)    # Close
        for path in ("/bin/wm/demos/counter", "/tests/sample.txt"):
            got = summed(dbg, path)
            res.check(f"...and sum agrees with the manifest for {path}",
                      got is not None and got[0] == crc_of(manifest, path), f"sum {got}")
        st = app.wait(lambda s: s.get("available") == 0)
        res.check("...the card says installed again", st and app.cards().get("demos", (0, ""))[1] == "installed",
                  f"{st} {app.cards().get('demos')}")

        # The list and its details pane.
        dbg.key("2", mods="ctrl")
        st = app.wait(lambda s: s.get("view") == "list")
        w = dbg.widgets(TITLE)
        res.check("Ctrl+2 shows the list with its details pane",
                  st and "details" in w and "detailbutton" in w, f"{st} {sorted(w)}")
        if "details" in w:
            d = w["details"]
            res.check("...and the pane is drawn",
                      ink(qmp, args.tmp, d["screen"]["x"], d["screen"]["y"], d["w"], d["h"]) > 200)
        dbg.key("1", mods="ctrl")
        app.wait(lambda s: s.get("view") == "cards")

        # The GPL switch, from the app.
        gpl = [cid for cid in staged if "GPL-" in open(os.path.join(STAGING, "usr/share/components",
                                                                       cid + ".component")).read()]
        w = dbg.widgets(TITLE)
        if "gpl" in w:
            click_rect(dbg, win, (w["gpl"]["x"], w["gpl"]["y"], w["gpl"]["w"], w["gpl"]["h"]))
        st = app.wait(lambda s: s.get("gpl") == 0)
        res.check("the switch turns software.show_gpl off",
                  "off" in dbg.send("sh config get software.show_gpl"), str(st))
        res.check("...and the GPL components' cards go",
                  st and st["cards"] == len(staged) - len(gpl) and not set(gpl) & set(app.cards()),
                  f"{st} {sorted(app.cards())} gpl={gpl}")
        w = dbg.widgets(TITLE)
        res.check("...with the banner's Show them", "showgpl" in w, str(sorted(w)))
        if "showgpl" in w:
            click_rect(dbg, win, (w["showgpl"]["x"], w["showgpl"]["y"], w["showgpl"]["w"], w["showgpl"]["h"]))
        st = app.wait(lambda s: s.get("gpl") == 1)
        res.check("Show them turns it back on", st and "on" in dbg.send("sh config get software.show_gpl"))
    finally:
        dbg.send("sh config set software.show_gpl on")
        if dbg.window(TITLE):
            dbg.close_window(TITLE)
        srv.shutdown()
        dbg.close()
    return res.finish("software_test")


if __name__ == "__main__":
    sys.exit(main())
