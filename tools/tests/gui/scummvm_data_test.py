#!/usr/bin/env python3
"""ScummVM's free-games card, the download, and ScummVM starting on it.

userland/apps/apps/scummvm/ (docs/sdl-design.md, stage 4): with no game
in /home/user/.local/share/games, the Start menu's ScummVM shows the card the four
freeware games download from (ui/uui_datacard.h, several at once); a
download is checked, unpacked, registered with ScummVM by ScummVM itself
(`--add`), and ScummVM's own launcher starts. Next time the front hands
over at once, and the game itself runs. A game a second ScummVM adds
while one is open survives the open one writing its list back (config
patch 0002), and one lost that way before the fix is added back, once.

THE DOWNLOAD NEVER LEAVES THIS MACHINE. The guest's
/etc/scummvm-games.conf gets `mirror=` pointed at a server this tool
runs on loopback (QEMU's 10.0.2.2), serving data/scummvm/lure-1.1.zip.

THE CONTROL COMES FIRST: the server hands out the zip with one byte
flipped, and the download must be REFUSED on its SHA-256 and leave no
folder behind -- not the game's, not its .part.

Needs an image carrying ScummVM (`make scummvm`, then `make iso`); says
so and fails without it. Without data/scummvm/lure-1.1.zip on the host
only the card and the control run (`tools/fetch/fetch_scummvm.py --games`
gets it).

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/scummvm_data_test.py
    python3 tools/lib/vm.py stop
"""

RUNNER = {
    "name": "scummvm",
    "covers": "ScummVM's free-games card, a refused tampered download, a real one registered "
              "with ScummVM, and a game running",
}


import argparse
import http.server
import os
import tempfile
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                  # noqa: E402
from harness import poll                         # noqa: E402
import port_guard                                # noqa: E402

try:
    from PIL import Image
except ImportError:
    print("scummvm_data_test: needs Pillow (pip install pillow)", file=sys.stderr)
    sys.exit(2)

REPO = toolpath.REPO
ZIP = os.path.join(REPO, "data", "scummvm", "lure-1.1.zip")
EXEC = "/bin/wm/apps/scummvm"

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(("  PASS  " if ok else "  FAIL  ") + name + (f"   {detail}" if detail else ""))
    return ok


# --- the mirror ---------------------------------------------------------------

class Mirror:
    def __init__(self):
        self.tamper = False
        self.hits = []
        mirror = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                mirror.hits.append(self.path[:60])
                if self.path != "/lure-1.1.zip":
                    self.send_response(404)
                    self.end_headers()
                    return
                if os.path.exists(ZIP):
                    data = bytearray(open(ZIP, "rb").read())
                else:
                    data = bytearray(b"PK\x03\x04" + b"\0" * 65536)
                if mirror.tamper or not os.path.exists(ZIP):
                    data[len(data) // 2] ^= 0x01
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(bytes(data))

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()


# --- the app, asked rather than guessed ------------------------------------

def logs(dbg):
    return [line for line in dbg.logs("scummvm:", clear=False) if "layout" not in line]


def wait_log(dbg, needle, timeout):
    hit = poll(lambda: [line for line in logs(dbg) if needle in line], timeout, step=0.4)
    return hit[-1] if hit else None


def layout(dbg):
    """The newest frame's controls: {name: [x, y, w, h, ...]}, content-relative."""
    lay = {}
    for line in dbg.logs("scummvm: layout ", clear=False):
        parts = line.split("scummvm: layout ", 1)[1].split()
        if parts[0] == "view":
            lay = {}
        lay[parts[0]] = parts[1:]
    return lay


def card_window(dbg):
    return next((w for w in dbg.windows() if w["title"] == "ScummVM"), None)


def press(dbg, name, lay=None):
    lay = lay or layout(dbg)
    win = card_window(dbg)
    v = lay.get(name)
    if not (win and v and len(v) >= 4):
        return False
    c = win["content"]
    x, y, w, h = (int(n) for n in v[:4])
    dbg.click(c["x"] + x + w // 2, c["y"] + y + h // 2)
    dbg.settle()
    return True


def row_named(lay, gid):
    for k, v in lay.items():
        if k.startswith("row") and v and v[-1] == gid:
            return k
    return None


def running(dbg):
    return [p for p in dbg.processes() if p.get("name") == "scummvm"]


def kill_all(dbg):
    for p in running(dbg):
        dbg.send(f"sh kill {p['pid']}")
    poll(lambda: not running(dbg), 15)


def clean(dbg):
    kill_all(dbg)
    # /usr/share/games too: ScummVM's front finds a game there after the
    # person's folder, and a disk downloaded into before 2026-11-25 has one.
    for d in ("bass", "fotaq", "lure", "drascula", "lure.part"):
        dbg.send(f"sh rm -r /home/user/.local/share/games/{d}")
        dbg.send(f"sh rm -r /usr/share/games/{d}")
    dbg.send("sh rm /home/user/.local/share/licenses/lure-of-the-temptress.txt")
    dbg.send("sh rm /home/user/.config/scummvm/scummvm.ini")


def selected_px(qmp, tmp, rect):
    """Pixels of the theme's selection colour inside `rect` (screen x y w h)."""
    shot = os.path.join(tmp, "scummvm_licence.png")
    qmp.screenshot(shot)
    x, y, w, h = rect
    box = Image.open(shot).convert("RGB").crop((x, y, x + w, y + h))
    return sum(1 for px in box.getdata() if px == (205, 220, 240))


def check_licence_selectable(dbg, qmp, tmp):
    """The licence is text a person can select and copy (userland/CLAUDE.md,
    rule 8): Ctrl+A paints all of it in the selection colour."""
    v = layout(dbg).get("licence")
    win = card_window(dbg)
    if not check("the licence's text reports where it is", bool(v and win), str(v)):
        return
    c = win["content"]
    rect = (c["x"] + int(v[0]), c["y"] + int(v[1]), int(v[2]), int(v[3]))
    before = selected_px(qmp, tmp, rect)
    qmp.combo(["ctrl", "a"])
    dbg.settle()
    after = selected_px(qmp, tmp, rect)
    check("Ctrl+A selects the licence's text (selection colour drawn under it)",
          before == 0 and after > rect[2] * 4, f"before={before} after={after}")


def run(dbg, qmp, mirror, tmp):
    if "scummvm" not in (dbg.send("sh ls /usr/bin") or "").split():
        check("the image carries /usr/bin/scummvm (make scummvm, then make iso)", False)
        return
    clean(dbg)
    dbg.write_lines("/etc/scummvm-games.conf", [f"mirror=http://10.0.2.2:{mirror.port}"])
    # A MACHINE UPDATED FROM BEFORE THE XDG FOLDERS SHIPPED has none, and
    # the front makes them -- a loop that started one character late made
    # them at /ome instead (17.11-22.11.2026), so ScummVM could write no
    # list and every game stayed out of it.
    for d in ("/home/user/.config", "/home/user/.local", "/home/user/.cache"):
        dbg.send(f"sh rm -r {d}")

    # --- 1. no game: the card ------------------------------------------
    dbg.logs("scummvm:", clear=True)
    dbg.send(f"gui spawn {EXEC}")
    if not check("with no game, ScummVM's front opens the free-games card",
                 bool(wait_log(dbg, "game data card", 30))):
        return
    dbg.settle()
    lay = layout(dbg)
    have = sorted(v[-1] for k, v in lay.items() if k.startswith("row"))
    check("...offering the four freeware games", have == ["bass", "drascula", "fotaq", "lure"], str(have))
    check("...the first one ticked", lay.get("checked") == ["0"], str(lay.get("checked")))

    # Untick Beneath a Steel Sky, tick Lure: several at once, by pointer.
    press(dbg, row_named(lay, "bass"), lay)
    press(dbg, row_named(lay, "lure"), lay)
    lay = layout(dbg)
    check("a click ticks and unticks a game", lay.get("checked") == ["2"], str(lay.get("checked")))

    # --- 2. THE CONTROL: a tampered zip is refused ------------------------
    mirror.tamper = True
    dbg.logs("scummvm:", clear=True)
    press(dbg, "primary", lay)
    check("Download shows Lure's licence, a notice to read",
          bool(wait_log(dbg, "licence for lure shown (notice)", 10)))
    dbg.settle()
    check_licence_selectable(dbg, qmp, tmp)
    press(dbg, "agree")
    said = wait_log(dbg, "download of lure failed", 60)
    check("a lure-1.1.zip with one byte flipped is REFUSED", bool(said) and "SHA-256" in said, str(said))
    left = (dbg.send("sh ls /home/user/.local/share/games") or "").split()
    check("...and no game folder is left, nor its .part",
          not any(f.startswith("lure") for f in left), str(left))
    check("...and the card is back to try again", bool(wait_log(dbg, "game data card", 10)))
    mirror.tamper = False

    if not os.path.exists(ZIP):
        print("\nscummvm_data_test: no data/scummvm/lure-1.1.zip on the host -- the download, "
              "the registration and the game were NOT checked (tools/fetch/fetch_scummvm.py --games)")
        kill_all(dbg)
        return

    # --- 3. the real download, unpacked, registered, ScummVM starts ---------
    dbg.settle()
    lay = layout(dbg)
    check("...with Lure still ticked", lay.get("checked") == ["2"], str(lay.get("checked")))
    dbg.logs("scummvm:", clear=True)
    press(dbg, "primary", lay)
    wait_log(dbg, "licence for lure shown", 10)
    dbg.settle()
    press(dbg, "agree")
    check("Lure downloads and its SHA-256 matches",
          bool(wait_log(dbg, "downloaded lure, checksum matches", 120)))
    check("...ScummVM adds it to its own list", bool(wait_log(dbg, "added lure to ScummVM's list", 60)))
    check("...and ScummVM starts", bool(wait_log(dbg, "starting ScummVM", 30)))
    # ITS WINDOW, not the log line: ScummVM in the card's process, after
    # the card's window, once never opened one (exec_window_test).
    launcher = poll(lambda: next((w for w in dbg.windows() if w["title"].startswith("ScummVM 2")), None), 60)
    check("...and its launcher's window opens after the card's", launcher is not None,
          str([w["title"] for w in dbg.windows()]))
    root = (dbg.send("sh ls -a /") or "").split()
    check("the front made the XDG folders in /home/user, and none at /ome",
          "ome/" not in root and "ome" not in root and
          "no such" not in (dbg.send("sh ls -a /home/user/.local/share") or "").lower(), str(root))
    ini = dbg.send("sh cat /home/user/.config/scummvm/scummvm.ini") or ""
    check("scummvm.ini, under XDG_CONFIG_HOME, names the game's folder",
          "path=/home/user/.local/share/games/lure" in ini, ini.strip()[-120:])
    exe = dbg.send("sh sum -a sha256 /home/user/.local/share/games/lure/Lure.exe") or ""
    check("the zip's lure/ folder is the game's folder (Lure.exe at its top)",
          "Lure.exe" in exe and "no such" not in exe.lower(), exe.strip()[:80])
    lic = dbg.send("sh cat /home/user/.local/share/licenses/lure-of-the-temptress.txt") or ""
    check("its own LICENSE.txt is saved as its licence", "Revolution Software" in lic, lic.strip()[:80])
    left = (dbg.send("sh ls /home/user/.local/share/games") or "").split()
    check("...and no .part is left", not any(f.endswith(".part") for f in left), str(left))

    # --- 4. next time: straight to ScummVM, then the game -------------------
    kill_all(dbg)
    dbg.logs("scummvm:", clear=True)
    dbg.send(f"gui spawn {EXEC}")
    launcher = poll(lambda: next((w for w in dbg.windows() if w["title"].startswith("ScummVM 2")), None), 60)
    check("with a game here, the front hands over to ScummVM's own launcher", launcher is not None,
          str([w["title"] for w in dbg.windows()]))
    check("...and opens no card", not wait_log(dbg, "game data card", 1))
    kill_all(dbg)

    # --- 4b. a folder ScummVM does not take is asked again, three times --------
    # Marking every attempt kept a game out of the list for good once an add
    # failed; now only a game ScummVM's list names is marked -- and a folder
    # it never takes is left alone after three tries, or every start of the
    # front would run a ScummVM for it.
    dbg.send("sh mkdir /home/user/.local/share/games/bass")
    dbg.send("sh write /home/user/.local/share/games/bass/readme.txt not a game")
    for attempt, says in (("first", "asked again"), ("second", "asked again"), ("third", "left alone")):
        dbg.logs("scummvm:", clear=True)
        dbg.send(f"gui spawn {EXEC}")
        check(f"a folder ScummVM does not detect is {says}, {attempt} start",
              bool(wait_log(dbg, "ScummVM did not list bass", 60))
              and bool(wait_log(dbg, says, 5)))
        poll(lambda: running(dbg), 30)
        kill_all(dbg)
        conf = dbg.send("sh cat /etc/scummvm-games.conf") or ""
        if attempt == "second":
            check("...and is not marked offered while tries are left", "added_bass" not in conf,
                  conf.strip()[-120:])
    check("...and is marked after the third, so no fourth ScummVM runs for it", "added_bass=yes" in conf,
          conf.strip()[-120:])
    check("...while Lure, which ScummVM listed, is", "added_lure=yes" in conf, conf.strip()[-120:])
    dbg.send("sh rm -r /home/user/.local/share/games/bass")
    dbg.send("gui spawn /usr/bin/scummvm lure")
    win = poll(lambda: next((w for w in dbg.windows() if w["title"].startswith("Lure of the Temptress")),
                            None), 60, step=0.5)
    if not check("`scummvm lure` opens the game's window", win is not None, str([w["title"] for w in dbg.windows()])):
        return
    c = win["content"]
    shot = os.path.join(tmp, "scummvm_lure.png")

    def lit():
        qmp.screenshot(shot, stable=False)
        box = Image.open(shot).convert("RGB").crop((c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"]))
        n = sum(1 for px in box.getdata() if max(px) > 60)
        return n if n > c["w"] * c["h"] // 100 else 0
    n = poll(lit, 30, step=1.0)
    check("...and the game draws in it (lit pixels, not a black window)", bool(n),
          f"{n} lit of {c['w'] * c['h']}")
    kill_all(dbg)

    # --- 5. an open ScummVM keeps a game another one added ----------------------
    # The front adds a game downloaded while ScummVM is open with a SECOND
    # ScummVM (`--add`), and the open one used to write its own list back over
    # it at its next flush -- its close, or any setting -- until config patch
    # 0002 (userland/third_party/scummvm/patches/). The game section is written
    # here as that second ScummVM would; a copy of Lure would not do, as
    # `--add` skips a game it has listed already.
    ini_path = "/home/user/.config/scummvm/scummvm.ini"
    dbg.send("gui spawn /usr/bin/scummvm")
    up = poll(lambda: next((w for w in dbg.windows() if w["title"].startswith("ScummVM 2")), None), 60)
    if check("ScummVM's launcher is open", up is not None, str([w["title"] for w in dbg.windows()])):
        before = dbg.send(f"sh cat {ini_path}") or ""
        for line in ("[lure-added-meanwhile]", "gameid=lure", "engineid=lure",
                     "description=Added while ScummVM was open", "path=/home/user/.local/share/games/lure"):
            dbg.send(f"sh append {ini_path} {line}")
        mid = dbg.send(f"sh cat {ini_path}") or ""
        check("...a game section is written to its list meanwhile, as `--add` writes one",
              "[lure-added-meanwhile]" in mid, mid.strip()[-200:])
        dbg.close_window(up["title"])
        poll(lambda: not running(dbg), 20)
        ini = dbg.send(f"sh cat {ini_path}") or ""
        check("a game another ScummVM wrote to the list while one was open survives the open "
              "one's close (which writes its list)",
              "[lure-added-meanwhile]" in ini and "[lure]" in ini,
              f"before {before.strip()!r}\n    after {ini.strip()!r}"[:1500])
    kill_all(dbg)

    # --- 6. a game lost that way before the fix is added back, once -------------
    # It stayed marked offered, so the front never asked again. Its first start
    # since unmarks a game on disk that the list lacks; a person removing it
    # after that is respected.
    dbg.send("sh rm /etc/scummvm-games.conf")
    dbg.send("sh write /etc/scummvm-games.conf added_lure=yes")
    dbg.send(f"sh write {ini_path} [scummvm]")
    dbg.logs("scummvm:", clear=True)
    dbg.send(f"gui spawn {EXEC}")
    check("a game marked added but missing from ScummVM's list is added again",
          bool(wait_log(dbg, "lure was marked added but is not in ScummVM's list", 60))
          and bool(wait_log(dbg, "added lure to ScummVM's list", 60)))
    poll(lambda: running(dbg), 30)
    kill_all(dbg)
    ini = dbg.send(f"sh cat {ini_path}") or ""
    check("...back in scummvm.ini", "path=/home/user/.local/share/games/lure" in ini, ini.strip()[-160:])
    dbg.send(f"sh write {ini_path} [scummvm]")
    dbg.logs("scummvm:", clear=True)
    dbg.send(f"gui spawn {EXEC}")
    poll(lambda: running(dbg), 30)
    check("...and only once: removed from the list again, it stays removed",
          not wait_log(dbg, "added lure", 8))
    kill_all(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=tempfile.gettempdir())
    args = ap.parse_args()
    port_guard.resolve_instance(args, "scummvm_data_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    mirror = Mirror()
    print(f"ScummVM's free games, from a mirror on loopback port {mirror.port}")
    try:
        with DebugConsole(args.sock) as dbg:
            dbg.settle()
            run(dbg, qmp, mirror, args.tmp)
            clean(dbg)
            dbg.send("sh rm /etc/scummvm-games.conf")
    finally:
        mirror.close()
    bad = [n for n, ok in checks if not ok]
    print(f"\nscummvm_data_test: {len(checks) - len(bad)} passed, {len(bad)} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
