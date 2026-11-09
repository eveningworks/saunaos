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
  - Remove on the Web page engine queues the Web Browser too (Needs=),
    and Keep on the Browser takes the engine's removal back with it;
  - Remove Demos and tests and the Browser, Apply, the confirm's Apply:
    `ls` no longer finds /bin/wm/demos/counter and the cards offer
    Install; the sidebar's Available count is 2;
  - the engine queued for removal, Install on the Browser: the engine's
    removal is taken back (the newest choice wins, the queue stays whole);
  - Install both, Apply: ONE fetch of the build's manifest for the two
    (the server counts it), `sum` agrees with the manifest, the cards
    say installed;
  - View > List (Ctrl+2): the rows and the details pane, its Remove
    button, and ink in the pane;
  - the switch: off writes software.show_gpl=off (read with `config`),
    the GPL components' cards go and the banner's "Show them" appears;
    "Show them" turns it back on;
  - an install whose build holds a /lib file (served for DOOM here) waits
    for the restart, and the app's done line says so;
  - a component with no icon: its tile is square with a letter INSIDE it
    (the whole name once spilled out of it, to the left);
  - under strace, the idle app makes no `setting` call -- the switch
    reaches it by the settings signal -- and `config set` still does.

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
import zlib

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


EXTRA_LIB = "/lib/zz-software-test.so"     # a library line: the add waits for the restart
EXTRA_BYTES = b"not a real library\n"


def serve(no_builds):
    """The handler, its manifest, and `ctl`: `requests` (every path asked
    for) and `extra_lib` (append EXTRA_LIB to DOOM's lines in a per-build
    manifest, so adding DOOM stages for the next boot)."""
    manifest = update_server.Manifest(STAGING)
    base = update_server.make_handler({"": manifest})
    ctl = {"requests": [], "extra_lib": False}

    class Quiet(base):
        def _say(self, *a):
            pass

        def do_GET(self):
            ctl["requests"].append(self.path)
            if no_builds and "/builds/" in self.path:
                self._send(404, b"build not on this server\n")
                return
            if ctl["extra_lib"] and "/builds/" in self.path:
                if self.path.endswith("/manifest"):
                    text, _ = manifest.build()
                    text += f"{zlib.crc32(EXTRA_BYTES)} {len(EXTRA_BYTES)} {EXTRA_LIB} component=doom\n"
                    self._send(200, text.encode())
                    return
                if self.path.endswith("/files" + EXTRA_LIB):
                    self._send(200, EXTRA_BYTES, "application/octet-stream")
                    return
            super().do_GET()

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, manifest, ctl


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
                return {"view": m.group(1), "cat": int(m.group(2)), "cards": int(m.group(3)), "queue": int(m.group(4)),
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


NO_ICON_DEF = "/usr/share/components/aatest.component"
NO_ICON = ["Name=Aardvark test tools", "Summary=A card with no icon", "Category=demos",
           "License=MIT", "Paths=/nonexistent-aatest"]


def tile_ink(qmp, tmp, x, y, w, h):
    """The icon tile in a card at (x, y, w, h): the bounding box of its
    colour (the one saturated block in the card's top-left), and how many
    near-white letter pixels lie in its MIDDLE. The whole name, centred
    on a tile it is wider than, starts left of the tile and is clipped to
    whole glyphs there: white on the white card, and the tile empty."""
    from PIL import Image
    p = os.path.join(tmp, f"software_tile_{os.getpid()}.png")
    qmp.screenshot(p)
    im = Image.open(p).convert("RGB").crop((x, y, x + w, y + h))
    px = im.load()
    W, H = im.size
    # The tile is the card's one block of a strong colour: blue-ish, red-ish...
    def strong(c):
        return max(c) - min(c) > 60 and sum(c) < 500
    pts = [(i, j) for j in range(H // 2) for i in range(W // 2) if strong(px[i, j])]
    if not pts:
        return None
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts)
    # The middle half only: the rounded corners show the card's white.
    qw, qh = (x1 - x0 + 1) // 4, (y1 - y0 + 1) // 4
    letter = sum(1 for j in range(y0 + qh, y1 + 1 - qh) for i in range(x0 + qw, x1 + 1 - qw)
                 if sum(px[i, j]) > 690)
    return {"x": x0, "y": y0, "w": x1 - x0 + 1, "h": y1 - y0 + 1, "letter": letter}


def setting_calls(dbg):
    return dbg.send("sh cat /tmp/software.trace").count("setting(")


def unstage_doom(dbg):
    """Undo an install left staged for the restart: the pending list goes,
    the staged files are put in place, and the test library goes."""
    dbg.send("sh rm /var/lib/update/pending")
    dbg.send("sh mv /bin/wm/apps/doom.upd /bin/wm/apps/doom")
    dbg.send("sh mv /usr/wm/applications/doom.desktop.upd /usr/wm/applications/doom.desktop")
    dbg.send(f"sh rm {EXTRA_LIB}.upd")


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
    srv, manifest, ctl = serve(args.positive_control)
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

        # A category row filters the cards. Its click selects the row in the
        # tree before the app hears of it, and the app once read that as
        # "no such category" and snapped back to All -- so ask the cards.
        r = app.rect("cats.games")
        if r:
            click_rect(dbg, win, r)
        st = app.wait(lambda s: s.get("cards") == 1)
        res.check("a click on Games shows its one card, DOOM",
                  r is not None and st and st["cat"] != 1 and list(app.cards()) == ["doom"],
                  f"{r} {st} {sorted(app.cards())}")
        r = app.rect("cats.all")
        if r:
            click_rect(dbg, win, r)
        st = app.wait(lambda s: s.get("cards") == len(staged))
        res.check("...and All brings every card back", r is not None and st and st["cat"] == 1, str(st))

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

        def footer(name):
            w = dbg.widgets(TITLE)
            if name in w:
                click_rect(dbg, win, (w[name]["x"], w[name]["y"], w[name]["w"], w[name]["h"]))
            return name in w

        def apply_all(n, timeout, enter=False):
            """Apply the queue of `n`, confirm, wait, Close: the done line.
            `enter`: confirm with the Enter key, as a person would, rather
            than a click on the dialog's Apply."""
            dbg.logs("software:", clear=True)
            footer("apply")
            asked = poll(lambda: any(f"software: ask apply {n}" in ln for ln in app.lines()), 10, 0.3)
            if enter:
                at = poll(lambda: app.rect("dialog.button", 0), 10, 0.3)
                qmp.send_key("ret")
            else:
                at = dialog_button(dbg, win, app)   # Apply: its rect came with the ask, so the log is not cleared
            done = poll(lambda: next((ln for ln in app.lines() if "software: apply done" in ln), None),
                        timeout, 0.5)
            dialog_button(dbg, win, app, at)    # Close
            return asked, done

        def state_of(cid):
            return app.cards().get(cid, (0, ""))[1]

        # Needs=: the engine takes the Browser with it -- and keeping the
        # Browser keeps the engine it needs.
        press("webengine")
        st = app.wait(lambda s: s.get("queue") == 2)
        res.check("removing the Web page engine queues the Web Browser too",
                  st and state_of("browser") == "remove", f"{st} {app.cards()}")
        press("browser")                   # Keep
        st = app.wait(lambda s: s.get("queue") == 0, 5)
        res.check("...and Keep on the Browser takes the engine's removal back too",
                  st and state_of("webengine") == "installed" and state_of("browser") == "installed",
                  f"{app.state()} {app.cards().get('webengine')} {app.cards().get('browser')}")
        footer("discard")
        app.wait(lambda s: s.get("queue") == 0)

        # Remove Demos and tests and the Browser, for real, in one Apply.
        press("demos")
        app.wait(lambda s: s.get("queue") == 1)
        press("browser")
        app.wait(lambda s: s.get("queue") == 2)
        asked, done = apply_all(2, 120)
        res.check("Apply asks first, with the steps", asked)
        res.check("the removals run", done and "apply done 2 of 2" in done, str(done))
        res.check("...and /bin/wm/demos/counter is gone, by ls",
                  "counter" not in dbg.send("sh ls /bin/wm/demos").split())
        st = app.wait(lambda s: s.get("available") == 2)
        res.check("...the card offers Install, the sidebar counts them available",
                  st and state_of("demos") == "available", f"{st} {app.cards().get('demos')}")

        # The engine queued for removal, then Install on the Browser that
        # needs it: the install wins, and the engine stays.
        press("webengine")
        app.wait(lambda s: s.get("queue") == 1)
        press("browser")
        st = app.wait(lambda s: s.get("queue") == 1 and state_of("browser") == "add", 5)
        res.check("Install on the Browser takes back the queued removal of the engine it needs",
                  st and state_of("webengine") == "installed",
                  f"{app.state()} {app.cards().get('webengine')} {app.cards().get('browser')}")
        footer("discard")
        app.wait(lambda s: s.get("queue") == 0)

        # Install both back, from the server, at this build: ONE fetch of
        # the build's manifest for the two.
        press("demos")
        app.wait(lambda s: s.get("queue") == 1)
        press("browser")
        app.wait(lambda s: s.get("queue") == 2)
        ctl["requests"].clear()
        asked, done = apply_all(2, 300)
        res.check("the installs run", done and "apply done 2 of 2" in done, str(done))
        fetched = [r for r in ctl["requests"] if "/builds/" in r and r.endswith("/manifest")]
        res.check("...fetching the build's manifest once for both", len(fetched) == 1, str(fetched))
        for path in ("/bin/wm/demos/counter", "/tests/sample.txt", "/bin/wm/apps/browser"):
            got = summed(dbg, path)
            res.check(f"...and sum agrees with the manifest for {path}",
                      got is not None and got[0] == crc_of(manifest, path), f"sum {got}")
        st = app.wait(lambda s: s.get("available") == 0)
        res.check("...the cards say installed again", st and state_of("demos") == "installed"
                  and state_of("browser") == "installed", f"{st} {app.cards()}")

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
        # The view is remembered: closed on the list, it opens on the list.
        dbg.close_window(TITLE)
        dbg.logs("software:", clear=True)
        win = dbg.spawn(APP, TITLE)
        st = app.wait(lambda s: s.get("view") == "list")
        res.check("closed on the list, it opens on the list",
                  st and "view=list" in (dbg.send("sh cat /etc/software-app.conf") or ""), str(st))
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

        # An install holding a library waits for the restart, and says so.
        press("doom")
        app.wait(lambda s: s.get("queue") == 1)
        asked, done = apply_all(1, 120, enter=True)
        res.check("Enter in the confirm dialog applies, as its Apply button does",
                  asked and done and "apply done 1 of 1" in done, str(done))
        app.wait(lambda s: state_of("doom") == "available")
        press("doom")
        app.wait(lambda s: s.get("queue") == 1)
        ctl["extra_lib"] = True
        asked, done = apply_all(1, 300)
        ctl["extra_lib"] = False
        res.check("an install holding a library says a restart finishes it",
                  done and "apply done 1 of 1" in done and "restart" in done, str(done))
        unstage_doom(dbg)
        dbg.close_window(TITLE)

        # A card with no icon: its tile carries an initial, not the name.
        for line in NO_ICON:
            dbg.send(f"sh tosh -c echo {line} >> {NO_ICON_DEF}")   # `sh` alone does not redirect
        dbg.logs("software:", clear=True)
        win = dbg.spawn(APP, TITLE)
        app.wait(lambda s: "aatest" in app.cards())
        i = app.cards().get("aatest", (None,))[0]
        r = app.rect("cards.item", i) if i is not None else None
        c = win["content"] if win else {"x": 0, "y": 0}
        tile = tile_ink(qmp, args.tmp, c["x"] + r[0], c["y"] + r[1], r[2], r[3]) if r else None
        res.check("a card with no icon draws a square tile with a letter IN it",
                  tile and abs(tile["w"] - tile["h"]) <= 2 and tile["letter"] > 5,
                  f"{tile} card {i} at {r}, cards {sorted(app.cards())}")
        dbg.close_window(TITLE)
        dbg.send(f"sh rm {NO_ICON_DEF}")

        # The switch flipped elsewhere reaches the app by the settings
        # signal, not by reading the setting on every tick.
        dbg.send("sh rm /tmp/software.trace")
        dbg.logs("software:", clear=True)
        dbg.send(f"gui spawn /bin/strace -e setting -o /tmp/software.trace {APP}")
        win = dbg.wait_window(TITLE)
        app.wait(lambda s: s.get("cards", 0) > 0)
        dbg.settle()
        n0 = setting_calls(dbg)
        grew = poll(lambda: setting_calls(dbg) - n0 >= 6 or None, 4, 0.5)
        res.check("idle, the app asks for no setting", win and not grew,
                  f"{n0} then {setting_calls(dbg)} setting calls")
        dbg.send("sh config set software.show_gpl off")
        st = app.wait(lambda s: s.get("gpl") == 0, 10)
        res.check("...and a switch flipped with `config` still reaches it", st, str(app.state()))
    finally:
        dbg.send("sh config set software.show_gpl on")
        dbg.send(f"sh rm {NO_ICON_DEF}")
        if dbg.window(TITLE):
            dbg.close_window(TITLE)
        srv.shutdown()
        dbg.close()
    return res.finish("software_test")


if __name__ == "__main__":
    sys.exit(main())
