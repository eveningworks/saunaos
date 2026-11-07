#!/usr/bin/env python3
"""Every rect and every pixel of a window, to compare two builds of it.

    tools/t ui_snapshot boot OUT                      # a fresh image of this build, every recipe
    tools/t ui_snapshot boot OUT -r notepad-options   # one recipe (repeatable)
    tools/t ui_snapshot take OUT -r files-options --instance 2   # a vm.py guest already up
    tools/t ui_snapshot take OUT --spawn /bin/wm/apps/calculator --title Calculator --prefix calculator
    tools/t ui_snapshot compare BEFORE AFTER
    tools/t ui_snapshot --list

WHY. Moving a window from C to a .ui file (docs/ui-files-design.md) is
meant to change nothing a person sees, and "measured identical to the C"
is the claim each conversion makes -- every rect in the layout report and
every pixel, in each state the window has. Every conversion re-derived
the script that took that measurement. So: `boot` a snapshot of the
build BEFORE the change, make it, `make iso`, `boot` a second, `compare`.

WHAT A SNAPSHOT IS, per recipe and per state (an Options page, or the one
window): the content's pixels (a PNG, the pointer parked at the screen's
corner first), the client's own widget map (`gui widgets`, keyed by
`uui_item.name`; empty for a dialog, which is why the next one exists),
and the window's `<prefix>: layout` lines as they stood when it opened.
A layout line is logged when a layout RUNS -- at open and on a resize --
so an Options window's later pages are measured by their pixels alone.

TWO TRAPS IT ENCODES. A snapshot taken TWICE of one build is the only way
to know what varies by itself (Screenshot's file-name preview shows the
clock), so `boot` takes two and prints what differs between them: that is
the noise, and a difference `compare` shows in the same box is not
evidence. And the guest boots COPIES of the image and the ISO, made in a
child that holds the test lock (tools/lib/tree_lock.py) only while it
copies -- the run itself reads nothing from the checkout, so sources stay
editable for its ten minutes.

`boot` uses `--instance` (default 3) for its guest; `take` drives one
already up (`vm.py start`). Recipes are this file's RECIPES table: a way
to open the window, its title, its log prefix and its states.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import port_guard  # noqa: E402

REPO = toolpath.REPO
UP, ENTER, F10, RIGHT, PRTSC, ESC = "0xf780", "0x0d", "0xf793", "0xf785", "0xf888", "0x1b"


def rect(dbg, prefix, key):
    for line in reversed(dbg.logs(f"{prefix}: layout {key} ", clear=False)):
        try:
            return [int(v) for v in line.split(f"layout {key} ", 1)[1].split()[:4]]
        except ValueError:
            return None
    return None


def wait(pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.3)
    return None


def click_in(dbg, win, r):
    c = win["content"]
    dbg.click(c["x"] + r[0] + r[2] // 2, c["y"] + r[1] + r[3] // 2)


def keys(dbg, *ks):
    for k in ks:
        dbg.send(f"gui key {k}")
        dbg.settle(0.3)


# --- how each window is opened ------------------------------------------------

def open_notepad(dbg, qmp):
    dbg.spawn("/bin/wm/apps/notepad", "untitled")
    dbg.settle(1)
    keys(dbg, F10, RIGHT, "o")               # Edit > Options...


def open_files(dbg, qmp):
    w = dbg.spawn("/bin/wm/apps/files", "File Manager")
    dbg.settle(1)
    click_in(dbg, w, wait(lambda: rect(dbg, "files", "toolbar.button 9")))   # See more
    keys(dbg, UP, UP, ENTER)                 # Close, then Options...


def open_terminal(dbg, qmp):
    dbg.spawn("/bin/wm/apps/uterm", "Terminal")
    dbg.settle(1.5)
    tb = dbg.widgets("Terminal")["tb"]
    line = wait(lambda: dbg.logs("uterm: layout tbbtn 3 ", clear=False))
    x, y, w, h = (int(v) for v in re.search(r"tbbtn 3 (\d+) (\d+) (\d+) (\d+)", line[-1]).groups())
    dbg.click(tb["screen"]["x"] - tb["x"] + x + w // 2, tb["screen"]["y"] - tb["y"] + y + h // 2)
    keys(dbg, UP, UP, ENTER)                 # the menu button: Exit, then Options...


def open_screenshot(dbg, qmp):
    dbg.send(f"gui key {PRTSC}")
    w = wait(lambda: next((x for x in dbg.windows()
                           if x.get("title") == "Screenshot" and x.get("state") == "fullscreen"), None))
    click_in(dbg, w, wait(lambda: rect(dbg, "screenshot", "gear")))


def open_retro(dbg, qmp):
    dbg.spawn("/bin/wm/apps/retrolauncher", "Retro Launcher")
    dbg.settle(1)
    dbg.send("gui resize 1200 380")          # wide enough for the whole toolbar
    time.sleep(2)
    click_in(dbg, dbg.window("Retro Launcher"), wait(lambda: rect(dbg, "retrolauncher", "toolbar.button 9")))


def spawner(path, title):
    def open_it(dbg, qmp):
        dbg.spawn(path, title)
        dbg.settle(1)
    return open_it


# name: (open, the window's title, its layout prefix, Options pages -- 0 for a window's one state)
RECIPES = {
    "notepad-options": (open_notepad, "Notepad Options", "options", 3),
    "files-options": (open_files, "File Manager Options", "options", 3),
    "terminal-options": (open_terminal, "Terminal Options", "options", 5),
    "screenshot-options": (open_screenshot, "Screenshot Options", "options", 3),
    "retro-options": (open_retro, "Retro Launcher Options", "retrolauncher options", 7),
}


# --- taking one -------------------------------------------------------------

def close_all(dbg):
    for _ in range(4):
        ws = [w for w in dbg.windows() if w.get("title")]
        if not ws:
            return
        for w in sorted(ws, key=lambda w: -w["z"]):
            # Screenshot's overlay closes on Esc; anything else as its X would.
            dbg.send(f"gui key {ESC}" if w.get("title") == "Screenshot" else "gui close %d" % w["z"])
            dbg.settle(0.3)
        time.sleep(1.5)
    print("ui_snapshot: still open:", [w["title"] for w in dbg.windows()])


def capture(dbg, qmp, out, title, png):
    from PIL import Image
    dbg.warp_cursor(qmp, 2, 2)               # its sprite alone makes two frames differ
    dbg.settle(0.8)
    time.sleep(0.8)
    dbg.settle(0.3)
    wid = dbg.widgets(title)
    shot = os.path.join(out, ".screen.png")
    qmp.screenshot(shot)                     # a SETTLED frame (qmp_test.py)
    c = dbg.window(title)["content"]
    Image.open(shot).convert("RGB").crop((c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"])).save(
        os.path.join(out, png))
    os.remove(shot)
    return {k: [v["x"], v["y"], v["w"], v["h"]] for k, v in wid.items()}


def snap(dbg, qmp, out, name, recipe):
    opener, title, prefix, pages = recipe
    dbg.logs("", clear=True)
    opener(dbg, qmp)
    ready = (lambda: dbg.window(title) if rect(dbg, prefix, "pages") else None) if pages else \
            (lambda: dbg.window(title))
    win = wait(ready, 15)
    if not win:
        print(f"ui_snapshot: {name}: no window '{title}' -- open: {[w['title'] for w in dbg.windows()]}")
        close_all(dbg)
        return None
    dbg.settle(0.5)
    time.sleep(0.5)
    res = {"window": [win["content"]["w"], win["content"]["h"]],
           "layout": sorted(set(l.split(f"{prefix}: layout ", 1)[1].strip()
                                for l in dbg.logs(f"{prefix}: layout ", clear=False)))}
    if not pages:
        res["state0"] = capture(dbg, qmp, out, title, f"{name}-0.png")
    else:
        pg = rect(dbg, prefix, "pages")
        rh = int(dbg.logs(f"{prefix}: layout pages.row_h ", clear=False)[-1].split("pages.row_h", 1)[1].split()[0])
        for p in range(pages):
            c = dbg.window(title)["content"]
            dbg.click(c["x"] + pg[0] + 30, c["y"] + pg[1] + p * rh + rh // 2)   # the sidebar's row
            res[f"state{p}"] = capture(dbg, qmp, out, title, f"{name}-{p}.png")
        dbg.send(f"gui key {ESC}")           # Cancel
        dbg.settle(0.8)
    close_all(dbg)
    return res


def take(out, recipes, sock, qmp_port):
    from gui_debug import DebugConsole, enter_gui
    from qmp_test import QMPSession
    os.makedirs(out, exist_ok=True)
    qmp = QMPSession(port=qmp_port)
    enter_gui(qmp, sock)                     # also turns desktop.layout_log on
    dbg = DebugConsole(sock)
    dbg.send("sh config set desktop.layout_log on")
    close_all(dbg)
    allres, ok = {}, True
    try:
        for name, recipe in recipes.items():
            allres[name] = snap(dbg, qmp, out, name, recipe)
            ok &= allres[name] is not None
            print(f"ui_snapshot: {name} {'taken' if allres[name] else 'FAILED'}", flush=True)
    finally:
        dbg.close()
        qmp.close()
    with open(os.path.join(out, "snapshot.json"), "w") as f:
        json.dump(allres, f, indent=1, sort_keys=True)
    return ok


# --- comparing two ----------------------------------------------------------

def compare(a, b, quiet=False):
    """The differences, one line each; [] when identical."""
    from PIL import Image, ImageChops
    la = json.load(open(os.path.join(a, "snapshot.json")))
    lb = json.load(open(os.path.join(b, "snapshot.json")))
    diffs = []
    for name in sorted(set(la) | set(lb)):
        sa, sb = la.get(name), lb.get(name)
        if not sa or not sb:
            diffs.append(f"{name}: not taken in {a if not sa else b}")
            continue
        if sa["window"] != sb["window"]:
            diffs.append(f"{name}: window {sa['window']} -> {sb['window']}")
        for x in sorted(set(sa["layout"]) ^ set(sb["layout"])):
            diffs.append(f"{name}: layout {'-' if x in sa['layout'] else '+'} {x}")
        for state in sorted(k for k in set(sa) | set(sb) if k.startswith("state")):
            wa, wb = sa.get(state) or {}, sb.get(state) or {}
            for k in sorted(set(wa) | set(wb)):
                if wa.get(k) != wb.get(k):
                    diffs.append(f"{name} {state}: {k} {wa.get(k)} -> {wb.get(k)}")
            png = f"{name}-{state[5:]}.png"
            pa, pb = os.path.join(a, png), os.path.join(b, png)
            if not (os.path.exists(pa) and os.path.exists(pb)):
                diffs.append(f"{png}: missing")
                continue
            ia, ib = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
            if ia.size != ib.size:
                diffs.append(f"{png}: {ia.size} -> {ib.size}")
                continue
            d = ImageChops.difference(ia, ib)
            box = d.getbbox()
            if box:
                n = sum(1 for px in d.get_flattened_data() if px != (0, 0, 0)) \
                    if hasattr(d, "get_flattened_data") else sum(1 for px in d.getdata() if px != (0, 0, 0))
                diffs.append(f"{png}: {n} pixels differ in {box}")
    if not quiet:
        for line in diffs:
            print(line)
        print("ui_snapshot: identical" if not diffs else f"ui_snapshot: {len(diffs)} difference(s)")
    return diffs


# --- booting a copy ---------------------------------------------------------

def prepare(work):
    """A fresh image and a copy of the ISO of THIS build, made under the test
    lock -- and only they: the guest reads nothing else from the checkout."""
    import tree_lock
    from fresh_disk import make_fresh
    tree_lock.hold("ui_snapshot prepare")
    iso = os.path.join(REPO, "saunaos.iso")
    if not os.path.exists(iso):
        sys.exit("ui_snapshot: no saunaos.iso -- run `make iso` first")
    make_fresh(os.path.join(work, "disk.img"))
    shutil.copyfile(iso, os.path.join(work, "saunaos.iso"))


def boot(out, recipes, instance, twice):
    work = tempfile.mkdtemp(prefix="ui_snapshot-")
    try:
        subprocess.run([sys.executable, os.path.abspath(__file__), "_prepare", work], check=True)
        vm = [sys.executable, toolpath.tool("vm.py"), "--instance", str(instance),
              "--disk", os.path.join(work, "disk.img"), "--iso", os.path.join(work, "saunaos.iso")]
        subprocess.run(vm + ["start"], check=True, stdout=subprocess.DEVNULL)
        try:
            sock, qmp_port = port_guard.instance_sock(instance), port_guard.instance_qmp(instance)
            ok = take(out, recipes, sock, qmp_port)
            if ok and twice:
                ok = take(out + ".again", recipes, sock, qmp_port)
                noise = compare(out, out + ".again", quiet=True)
                print("ui_snapshot: two snapshots of this build agree" if not noise else
                      "ui_snapshot: NOISE -- what differs between two snapshots of this build:")
                for line in noise:
                    print("  " + line)
        finally:
            subprocess.run(vm + ["stop"], stdout=subprocess.DEVNULL)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return ok


def pick(args):
    if args.spawn:
        if not args.title or not args.prefix:
            sys.exit("ui_snapshot: --spawn needs --title and --prefix")
        return {os.path.basename(args.spawn.split()[0]): (spawner(args.spawn, args.title), args.title,
                                                          args.prefix, 0)}
    names = args.recipe or list(RECIPES)
    bad = [n for n in names if n not in RECIPES]
    if bad:
        sys.exit(f"ui_snapshot: no recipe {', '.join(bad)} -- --list names them")
    return {n: RECIPES[n] for n in names}


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "_prepare":
        return prepare(sys.argv[2])
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true", help="name every recipe")
    sub = ap.add_subparsers(dest="cmd")
    for cmd in ("boot", "take"):
        p = sub.add_parser(cmd, help="boot a fresh image of this build and take it"
                           if cmd == "boot" else "take it from a vm.py guest already up")
        p.add_argument("out", help="the snapshot's directory")
        p.add_argument("-r", "--recipe", action="append", help="a recipe (repeatable; default every one)")
        p.add_argument("--spawn", help="a window instead: the program to start")
        p.add_argument("--title", help="with --spawn: its window's title")
        p.add_argument("--prefix", help="with --spawn: its layout lines' prefix")
        if cmd == "boot":
            p.add_argument("--instance", type=int, default=3, help="the guest's slot (default 3)")
            p.add_argument("--once", action="store_true", help="one snapshot, no noise measurement")
        else:
            port_guard.add_instance_args(p)
    c = sub.add_parser("compare", help="what differs between two snapshots")
    c.add_argument("a")
    c.add_argument("b")
    args = ap.parse_args()
    if args.list:
        for n, (_, title, prefix, pages) in RECIPES.items():
            print(f"{n:20} {title!r} ({pages} pages)" if pages else f"{n:20} {title!r}")
        return 0
    if args.cmd == "compare":
        return 1 if compare(args.a, args.b) else 0
    if args.cmd == "take":
        port_guard.resolve_instance(args, "ui_snapshot")
        return 0 if take(args.out, pick(args), args.sock, args.qmp_port) else 1
    if args.cmd == "boot":
        return 0 if boot(args.out, pick(args), args.instance, not args.once) else 1
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
