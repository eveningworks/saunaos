#!/usr/bin/env python3
"""The Web Browser: NetSurf's pages drawn in a toy-os window, and driven.

WHAT IT CHECKS, and what a broken version would still pass:

  - it opens on the start page and REPORTS it loaded: the page's address,
    not loading, and an extent taller than the view (the start page
    scrolls) -- a window that never laid a page out fails here;
  - the page is DRAWN, read in pixels of a settled frame: the start
    page's header band is its own blue, with white text in it, and a
    control rect lower down (the page's grey body) has none of that blue
    -- "it responds" is not "it is drawn";
  - an address typed after Ctrl+L is followed: the reported address and
    the title-bar TAB'S LABEL both become the new page's, and its table
    borders are drawn in black;
  - Back (Alt+Left) returns to the page before;
  - a CLICK on a link follows it: /tests/web/links.html's block link fills
    a known band at the top of the page, so the click needs no guessed
    coordinates, and the page it leads to must be the one loaded after;
  - the wheel scrolls a long page, by the scroll offset the app reports;
  - Ctrl+T opens a second tab and Ctrl+W closes it again.

Geometry and state are the app's own report (`browser: layout page ...`,
`browser: tab ...`), never re-derived here.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

APP = "/bin/wm/apps/browser"
HOME = "file:///usr/share/netsurf/start.html"
HEADER_BLUE = (50, 90, 160)        # start.html's .top background, #325aa0
HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "-": "0x2d", "_": "0x5f"}

_res = Results()
check = _res.check
_st = {}


def state(dbg):
    for line in dbg.logs("browser:", clear=True):
        m = re.search(r"browser: layout (\S+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _st[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"browser: tab (\d+)/(\d+) loading (\d) extent (\d+) (\d+) "
                      r"scroll (-?\d+) (-?\d+) url (\S*)", line)
        if m:
            g = m.groups()
            _st.update(cur=int(g[0]), tabs=int(g[1]), loading=int(g[2]), ext_w=int(g[3]),
                       ext_h=int(g[4]), sx=int(g[5]), sy=int(g[6]), url=g[7])
    return _st


def wait(dbg, ok, timeout=12.0):
    deadline = time.time() + timeout
    st = state(dbg)
    while not ok(st) and time.time() < deadline:
        time.sleep(0.2)
        st = state(dbg)
    return st


def window(dbg):
    wins = [w for w in dbg.windows() if w["title"].endswith("Web Browser")]
    return wins[-1] if wins else None


def type_text(dbg, text):
    for ch in text:
        dbg.send(f"gui key {HEX.get(ch, ch)}")


def go(dbg, path):
    dbg.send("gui key 0x0c ctrl")      # Ctrl+L: to the address, all selected
    type_text(dbg, path)
    dbg.send("gui key 0x0d")


def loaded(url):
    return lambda s: s.get("url") == url and s.get("loading") == 0 and s.get("ext_h", 0) > 0


def count(im, rect, pred):
    x, y, w, h = rect
    crop = im.crop((x, y, x + w, y + h)).convert("RGB")
    return sum(1 for p in crop.get_flattened_data() if pred(p))


def near(c, tol=24):
    return lambda p: all(abs(p[i] - c[i]) <= tol for i in range(3))


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "browser_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("browser (NetSurf in a window: drawing, addresses, links, history, scrolling, tabs)")

    old = window(dbg)
    if old:
        dbg.send(f"gui close {old['z']}")
        dbg.settle(1.0)
    dbg.logs("browser:", clear=True)
    # By SUFFIX, after the page has loaded: the title is "Web Browser"
    # only until the first page names itself ("Start - Web Browser"),
    # and a fast load renames it before spawn()'s exact match ever sees it.
    dbg.spawn(APP)
    st = wait(dbg, lambda s: "page" in s and loaded(HOME)(s))
    dbg.settle(1.0)
    win = window(dbg)
    if not check("it opens on the start page, loaded", win and loaded(HOME)(st),
                 f"url={st.get('url')} loading={st.get('loading')} ext_h={st.get('ext_h')}"):
        return finish()
    page = st["page"]
    check("...a page taller than the view, as the start page is", st["ext_h"] > page[3],
          f"extent {st['ext_h']} vs view {page[3]}")

    from PIL import Image
    win = window(dbg)
    c = win["content"]
    at = lambda r: (c["x"] + r[0], c["y"] + r[1], r[2], r[3])

    def shot(name):
        dbg.settle(1.0)
        p = os.path.join(args.tmp, f"browser_{name}_{os.getpid()}.png")
        qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    im = shot("home")
    band = at((page[0] + 10, page[1] + 10, page[2] // 2, 60))
    blue = count(im, band, near(HEADER_BLUE))
    white = count(im, band, lambda p: min(p) > 225)
    # Right of the cards, below the band: bare body grey. NOT the cards
    # themselves -- their link-blue headings (#2f5d9c) sit within the
    # tolerance of the band's #325aa0.
    ctl = count(im, at((page[0] + page[2] * 3 // 4, page[1] + 230, page[2] // 5, 60)),
                near(HEADER_BLUE))
    check("the start page's header band is drawn in its blue", blue > 2000, f"{blue} px")
    check("...with its heading in white text on it", white > 80, f"{white} px")
    check("...and the body below it has none of that blue (control)", ctl < 50, f"{ctl} px")

    go(dbg, "/tests/web/table.html")
    st = wait(dbg, loaded("file:///tests/web/table.html"))
    check("an address typed after Ctrl+L is followed", st.get("url") == "file:///tests/web/table.html",
          f"url={st.get('url')}")
    win = window(dbg)
    labels = [t.get("label") for t in (win or {}).get("tabs", [])]
    check("...and the tab is labelled with the page's title", "Tables" in labels, f"tabs {labels}")
    im = shot("table")
    black = count(im, at(page), lambda p: max(p) < 60)
    check("...whose table borders are drawn", black > 500, f"{black} dark px")

    dbg.send("gui key 0xF784 alt")     # Alt+Left
    st = wait(dbg, loaded(HOME))
    check("Back returns to the start page", st.get("url") == HOME, f"url={st.get('url')}")

    go(dbg, "/tests/web/links.html")
    st = wait(dbg, loaded("file:///tests/web/links.html"))
    dbg.send("gui click %d %d" % (c["x"] + page[0] + 60, c["y"] + page[1] + 60))
    st = wait(dbg, loaded("file:///tests/web/table.html"))
    check("a click on a link follows it", st.get("url") == "file:///tests/web/table.html",
          f"url={st.get('url')}")

    go(dbg, "/tests/web/text.html")
    st = wait(dbg, loaded("file:///tests/web/text.html"))
    dbg.move(c["x"] + page[0] + page[2] // 2, c["y"] + page[1] + page[3] // 2)
    for _ in range(3):
        dbg.wheel(-1)
    st = wait(dbg, lambda s: s.get("sy", 0) > 0, timeout=4)
    check("the wheel scrolls a long page", st.get("sy", 0) > 0, f"scroll y {st.get('sy')}")

    dbg.send("gui key 0x14 ctrl")      # Ctrl+T
    st = wait(dbg, lambda s: s.get("tabs") == 2 and loaded(HOME)(s))
    check("Ctrl+T opens a second tab, on the start page", st.get("tabs") == 2 and st.get("cur") == 1,
          f"tabs={st.get('tabs')} cur={st.get('cur')}")
    dbg.send("gui key 0x17 ctrl")      # Ctrl+W
    st = wait(dbg, lambda s: s.get("tabs") == 1, timeout=6)
    check("...and Ctrl+W closes it, back to the first", st.get("tabs") == 1 and
          st.get("url") == "file:///tests/web/text.html", f"tabs={st.get('tabs')} url={st.get('url')}")
    return finish()


def finish():
    return _res.finish("browser_test")


if __name__ == "__main__":
    sys.exit(main())
