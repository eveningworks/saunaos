#!/usr/bin/env python3
"""The File Manager's Network rows: Sign in, Add network share, and a share that does not answer.

tools/smb_test.py proves the shares work from a shell; this is what a
person does with them -- click a share that wants a password and get
the Sign-in dialog, type it and land in the folder, add a share with the
Add dialog, and be told when one does not answer.

NOTHING LEAVES THIS MACHINE: Samba in Docker on 127.0.0.1
(tools/samba_server.py), reached as SLIRP's 10.0.2.2. Signing is `auto`
here so the anonymous `public` share can be added and opened; the
dialects and mandatory signing are smb_hostcheck.py's and smb_test.py's.

DRAWN, NOT JUST RESPONDING (CLAUDE.md's GUI-test traps): a share row is
found by its icon's teal pixels in the tree -- so a tree that never drew
the Network group cannot be clicked into passing -- and the Sign-in
dialog must put its white card and its blue padlock on the screen, not
merely log that it opened. Each step then waits on the File Manager's
own `files: netshare` log line, and the folder it lands in must hold
what the fixture wrote.

  python3 tools/fm_network_test.py [--instance N] [--positive-control]

--positive-control types a WRONG password: the folder must never open,
so the run must go red.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import port_guard  # noqa: E402
import samba_server  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from harness import Results, copy_disk  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from PIL import Image  # noqa: E402

VM = os.path.join(HERE, "vm.py")


def tealish(px):
    """The share icon's cable at the tree's icon size, blended: green and
    blue well above red, and CLOSE to each other -- within 15, or the
    Downloads place's sky-blue tile (46, 140, 190), its edge blended into
    the pane, passes for teal."""
    r, g, b = px[:3]
    return g - r > 45 and b - r > 40 and abs(g - b) < 15 and r < 150


def share_rows(png, tree):
    """The y of each Network row in the tree, top to bottom. A share's
    icon ends in a cable BAR -- a run of teal at most four rows tall --
    which tells it apart from the Desktop place's solid teal tile."""
    im = Image.open(png).convert("RGB")
    sx, sy, w, h = tree["screen"]["x"], tree["screen"]["y"], tree["w"], tree["h"]
    hot = [sum(tealish(im.getpixel((x, y))) for x in range(sx, sx + min(w, 60))) >= 6
           for y in range(sy, sy + h)]
    rows, y = [], 0
    while y < h:
        if not hot[y]:
            y += 1
            continue
        start = y
        while y < h and hot[y]:
            y += 1
        if y - start <= 4:
            rows.append(sy + start - 4)     # just above the bar: inside the row
    return rows


def add_row(png, tree, below):
    """The y of the first icon under `below` in the tree's icon column --
    the Add network share row, which is the tree's last. Found by its
    drawn "+" rather than by a row pitch, which is font-derived."""
    im = Image.open(png).convert("RGB")
    sx = tree["screen"]["x"]
    ys = []
    for y in range(below + 10, min(below + 80, tree["screen"]["y"] + tree["h"])):
        dark = sum(sum(im.getpixel((x, y))) < 450 for x in range(sx + 8, sx + 36))
        if dark >= 2:
            ys.append(y)
        elif ys:
            break
    return (ys[0] + ys[-1]) // 2 if ys else None


def dialog_drawn(png, win, tiles=(0, 0)):
    """(white pixels, blue padlock pixels) over the whole window. A modal
    DIMS what is behind it, so the listing's white falls away and only
    the card stays white; the padlock is the one blue thing on it."""
    im = Image.open(png).convert("RGB")
    white = blue = 0
    # Blue is NOT counted in `tiles`, the left pane's icon column (x0, x1):
    # its place tiles (Downloads is sky blue) would swamp the padlock's
    # gain -- and the card may overlap the pane, so only that column goes.
    for y in range(win["y"], win["y"] + win["h"], 2):
        for x in range(win["x"], win["x"] + win["w"], 2):
            r, g, b = im.getpixel((x, y))
            white += (r, g, b) == (255, 255, 255)
            blue += not tiles[0] <= x < tiles[1] and b > 180 and b - r > 80 and b - g > 30
    return white, blue


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    inst = port_guard.find_free_instance() if args.instance == "auto" else int(args.instance)
    if inst is None:
        sys.exit("fm_network_test: no free VM slot")
    print(f"fm_network_test: VM slot {inst}"
          + ("  [POSITIVE CONTROL -- a wrong password, the folder must not open]" if args.positive_control else ""))
    res = Results()
    tmp = tempfile.mkdtemp(prefix="fmnet.")
    disk = copy_disk(os.path.join(REPO, "disk.img"), os.path.join(tmp, "fmnet.img"))
    sock = port_guard.instance_sock(inst)

    def vm(*a, timeout=300):
        r = subprocess.run([sys.executable, VM, "--instance", str(inst), "--disk", disk, *a],
                           capture_output=True, text=True, timeout=timeout, errors="replace")
        return r.stdout + r.stderr

    with samba_server.SambaServer(signing="auto") as srv:
        base = f"//10.0.2.2:{srv.port}"
        try:
            if "ready" not in vm("start"):
                res.check("the guest boots", False)
                return res.finish("fm_network_test")
            q = QMPSession(port=port_guard.instance_qmp(inst))
            enter_gui(q, sock=sock)
            dc = DebugConsole(sock)

            def wait_log(pattern, timeout=20):
                end, seen = time.time() + timeout, ""
                while time.time() < end:
                    seen += "\n".join(dc.logs("files: netshare"))
                    m = re.search(pattern, seen)
                    if m:
                        return m
                    time.sleep(0.5)
                return None

            def typ(s):
                for ch in s:
                    dc.key("0x20" if ch == " " else ch, settle=False)
                dc.settle()

            def shot(name):
                p = os.path.join(tmp, name + ".png")
                q.screenshot(p)
                return p

            dc.send("sh mkdir /etc/mounts.d")
            dc.send(f"sh write /etc/mounts.d/media Source={base}/media")
            dc.send(f"sh append /etc/mounts.d/media User={srv.USER}")
            dc.logs("files:")
            dc.open_app("File Manager")
            win = dc.window_settled("File Manager") or dc.window("File Manager")
            tree = dc.widgets("File Manager").get("tree")
            rows = []
            for _ in range(20):          # smbfsd mounts it within a couple of seconds
                rows = share_rows(shot("tree"), tree)
                if rows:
                    break
                time.sleep(0.5)
            res.check("the Network group draws the saved share", len(rows) == 1, f"rows at {rows}")
            if not rows:
                return res.finish("fm_network_test")

            print("sign in")
            before = dialog_drawn(shot("before"), win, (tree["screen"]["x"], tree["screen"]["x"] + 48))
            dc.click(tree["screen"]["x"] + 40, rows[0])
            res.check("a share that wants a password asks for one",
                      wait_log(r"netshare signin /net/media") is not None)
            dc.settle()
            time.sleep(0.5)
            white, blue = dialog_drawn(shot("signin"), win, (tree["screen"]["x"], tree["screen"]["x"] + 48))
            res.check("the Sign-in dialog is drawn: the window dims, the padlock shows",
                      white < before[0] * 0.85 and blue > before[1] + 40,
                      f"white {before[0]}->{white}, blue {before[1]}->{blue}")
            typ("wrong horse" if args.positive_control else srv.PASSWORD)
            dc.key("0x0d")
            m = wait_log(r"netshare open /net/media items (\d+)")
            want = len(srv.listing("media"))
            res.check("signed in, the File Manager opens the share whole",
                      m is not None and int(m.group(1)) in (want, want + 1),
                      m.group(0) if m else "never opened")

            print("add a share")
            p2 = shot("tree2")
            rows = share_rows(p2, tree)
            y_add = add_row(p2, tree, rows[-1]) if rows else None
            if y_add:
                dc.click(tree["screen"]["x"] + 60, y_add)
            ok = y_add is not None and wait_log(r"netshare add") is not None
            res.check("the Add network share row opens the dialog", ok)
            if ok:
                typ(f"{base}/public")
                time.sleep(1)
                dc.key("0x0d")
                m = wait_log(r"netshare open /net/public items (\d+)")
                res.check("an added share is saved, mounted and opened",
                          m is not None and int(m.group(1)) in (1, 2), m.group(0) if m else "never opened")
                out = dc.send("sh cat /etc/mounts.d/public") or ""
                res.check("...from a file in /etc/mounts.d", f"Source={base}/public" in out, out[-200:])

            print("Properties of a share")
            rows = share_rows(shot("tree2p"), tree)
            props = ""
            if rows:
                dc.logs("properties:")
                dc.rclick(tree["screen"]["x"] + 40, rows[0])
                dc.key("0xF780")                 # Up from nothing: the last row, Properties
                dc.key("0x0d")
                for _ in range(30):
                    props += "\n".join(dc.logs("properties:"))
                    if "properties: row Space=" in props:
                        break
                    time.sleep(0.5)
            res.check("Properties on a share's row opens its volume page: address, state, sign-in",
                      "properties: volume /net/media share" in props
                      and f"properties: row Address=smb:{base}/media" in props
                      and "properties: row Status=Connected" in props
                      and f"properties: row Signed in as={srv.USER}" in props, props[-400:])
            for w2 in dc.windows():
                if w2["title"].endswith(" Properties"):
                    dc.close_window(w2["title"])

            print("drag a share above another")
            rows = share_rows(shot("tree2d"), tree)
            if len(rows) >= 2:
                x = tree["screen"]["x"] + 60
                dc.warp_cursor(q, x, rows[1] + 6)
                q.mouse_down()
                time.sleep(0.2)
                pitch = rows[1] - rows[0]          # one row: the upper third of row 0 is its gap above
                for yy in range(rows[1] + 6, rows[0] - pitch // 3, -4):
                    dc.warp_cursor(q, x, yy)
                q.mouse_up()
            conf = ""
            for _ in range(20):
                conf = dc.send("sh cat /etc/places.conf") or ""
                if "dev:/net/public" in conf and "dev:/net/media" in conf:
                    break
                time.sleep(0.5)
            moved = ("dev:/net/public" in conf and "dev:/net/media" in conf
                     and conf.index("dev:/net/public") < conf.index("dev:/net/media"))
            res.check("a share dragged above the other moves there (dev: lines in /etc/places.conf)",
                      moved, conf[-300:])
            public_row = 0 if moved else 1         # Forget below aims at `public`, wherever it is now

            print("forget a share from its row's menu")
            rows = share_rows(shot("tree2b"), tree)
            forgot = False
            if len(rows) >= 2:
                dc.rclick(tree["screen"]["x"] + 40, rows[public_row])
                # An app's own menu is not `gui ctxmenu`'s (that is the
                # compositor's): Up from nothing is its LAST row,
                # Properties; once more is Forget (fm_places.c's order).
                dc.key("0xF780")
                dc.key("0xF780")
                dc.key("0x0d")
                if True:
                    for _ in range(20):
                        out = dc.send("sh ls /etc/mounts.d") or ""
                        if "public" not in out:
                            forgot = True
                            break
                        time.sleep(0.5)
            res.check("Forget this share, on the row's own menu, removes the saved share", forgot)

            print("an smb:// address in the address bar")
            dc.key("0x0c")                       # Ctrl+L: the address bar as text
            typ(f"smb://10.0.2.2:{srv.port}/public")
            dc.key("0x0d")
            m = wait_log(r"netshare address smb://\S+ -> (/net/\S+)")
            res.check("an smb:// address opens the share, mounted for the session",
                      m is not None and m.group(1).startswith("/net/public"), m.group(0) if m else "nothing")
            time.sleep(1)
            dc.settle()
            bar = dc.widgets("File Manager").get("netbar")
            ok = False
            if bar and bar.get("w", 0) > 0:
                # The Save button: the accent-filled run at the bar's right.
                im = Image.open(shot("netbar")).convert("RGB")
                bx, by = bar["screen"]["x"], bar["screen"]["y"]
                hits = [(x, y) for y in range(by, by + bar["h"]) for x in range(bx + bar["w"] // 2, bx + bar["w"])
                        if (lambda p: p[2] > 120 and p[2] - p[0] > 60 and p[0] < 110)(im.getpixel((x, y)))]
                if hits:
                    dc.click(sum(h[0] for h in hits) // len(hits), sum(h[1] for h in hits) // len(hits))
                    ok = wait_log(r"netshare save /net/public\S* rc 0") is not None
            res.check("the session bar's Save to Network saves it", ok, f"bar {bar}")
            name = m.group(1).split("/")[2] if m else "public"
            out = dc.send(f"sh cat /etc/mounts.d/{name}") or ""
            res.check("...as a file in /etc/mounts.d", f"Source=//10.0.2.2:{srv.port}/public" in out, out[-200:])

            print("a share that does not answer")
            dc.send("sh write /etc/mounts.d/dead Source=//10.0.2.2:1/nothing")
            rows = []
            for _ in range(20):
                rows = share_rows(shot("tree3"), tree)
                if len(rows) >= 3:
                    break
                time.sleep(0.5)
            if len(rows) >= 3:
                dc.click(tree["screen"]["x"] + 40, rows[-1])
                res.check("an unreachable share says so",
                          wait_log(r"netshare offline /net/dead state 5") is not None)
            else:
                res.check("the third share's row appears", False, f"rows {rows}")
        finally:
            vm("stop")
            subprocess.run(["rm", "-rf", tmp])
    rc = res.finish("fm_network_test")
    if args.positive_control:
        if rc:
            print("fm_network_test: positive control went red, as it must")
            return 0
        print("fm_network_test: POSITIVE CONTROL STAYED GREEN")
        return 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
