#!/usr/bin/env python3
"""The Web Browser: NetSurf's pages drawn in a toy-os window, and driven.

WHAT IT CHECKS, and what a broken version would still pass:

  - it opens on the start page and REPORTS it loaded: the page's address,
    not loading, and an extent at least the view's (NetSurf's root box is
    never shorter) -- a window that never laid a page out reports 0;
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
  - Ctrl+T opens a second tab and Ctrl+W closes it again;
  - /tests/web/fonts.html draws serif, italic and monospace italic each
    in its own face -- the same word's ink differs from the upright or
    sans row beside it -- while a row repeated, and "Times New Roman"
    against `serif`, match (the controls: a broken version that drew
    every row the same would pass "it differs" nowhere);
  - /tests/web/photos.html's progressive JPEG (four colour quadrants)
    and 2-bit palette Adam7 PNG (four bands) are DRAWN, each region its
    own colour in the pixels -- decoded by lib/uimg.h in a thread, as
    both are over the in-place threshold -- and the page beside them
    stays white (control).

OVER THE NETWORK (stage 2), against tools/web_server.py on this machine
through SLIRP's 10.0.2.2 -- skipped on bare metal, where that address is
nobody:

  - a typed host:port/path is fetched over http, and the page's own
    stylesheet -- a SEPARATE request -- is applied: net.css's red rule is
    in the pixels, and none of it lower down (control);
  - a cookie the server set is sent back on the next page, and kept in
    /var/lib/browser/cookies when the window closes;
  - words that are not an address become a search (--search-url points
    it at the same server, so nothing leaves the machine);
  - the server saw FEWER connections than requests: they were reused;
  - https with a test CA (fetched into /etc/ssl/certs by wget) loads, and
    an expired certificate stops at NetSurf's "Privacy error" page.

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
from web_server import WebServer, make_ca, GATEWAY  # noqa: E402

APP = "/bin/wm/apps/browser"
HOME = "file:///usr/share/netsurf/start.html"
HEADER_BLUE = (50, 90, 160)        # start.html's .top background, #325aa0
HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "-": "0x2d", "_": "0x5f", ":": "0x3a",
       "?": "0x3f", "=": "0x3d"}
NET_RED = (192, 48, 48)            # net.css's h1 border, #c03030
WEB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "data", "tests", "web")

_res = Results()
check = _res.check
_st = {}


def state(dbg):
    for line in dbg.logs("browser:", clear=True):
        m = re.search(r"browser: layout (\S+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _st[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"browser: tab (\d+)/(\d+) loading (\d) extent (\d+) (\d+) "
                      r"scroll (-?\d+) (-?\d+) len (\d+) url (\S*)", line)
        if m:
            g = m.groups()
            # The app reports at most 72 bytes of the address: `url` is
            # then a prefix, and `urllen` its real length.
            _st.update(cur=int(g[0]), tabs=int(g[1]), loading=int(g[2]), ext_w=int(g[3]),
                       ext_h=int(g[4]), sx=int(g[5]), sy=int(g[6]), urllen=int(g[7]), url=g[8])
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
    # A pause every 30 keys: a long burst outruns the app's event queue,
    # and the key lost is as likely to be the Enter after it.
    for i, ch in enumerate(text):
        dbg.send(f"gui key {HEX.get(ch, ch)}")
        if i % 30 == 29:
            dbg.settle(0.3)


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

    # The network half's servers, started before the app so its search
    # can be pointed at one. Not on bare metal: 10.0.2.2 is SLIRP's.
    net = None
    if not os.environ.get("TOYOS_REMOTE_HOST"):
        import shutil
        import tempfile
        d = tempfile.mkdtemp(prefix="browser_test.")
        web = os.path.join(d, "web")
        shutil.copytree(WEB, web)
        certs = make_ca(os.path.join(d, "ca")) if shutil.which("openssl") else None
        if certs:
            shutil.copy(certs["ca"], os.path.join(web, "ca.pem"))
        net = {"dir": d, "http": WebServer(web, 0), "certs": certs}
        if certs:
            net["good"] = WebServer(web, 0, tls=certs["good"])
            net["expired"] = WebServer(web, 0, tls=certs["expired"])
    try:
        return run(dbg, qmp, args, net)
    finally:
        if net:
            for k in ("http", "good", "expired"):
                if k in net:
                    net[k].shutdown()
            import shutil
            shutil.rmtree(net["dir"], ignore_errors=True)


def run(dbg, qmp, args, net):
    old = window(dbg)
    if old:
        dbg.send(f"gui close {old['z']}")
        dbg.settle(1.0)
    dbg.logs("browser:", clear=True)
    # By SUFFIX, after the page has loaded: the title is "Web Browser"
    # only until the first page names itself ("Start - Web Browser"),
    # and a fast load renames it before spawn()'s exact match ever sees it.
    argv = APP
    if net:
        argv += f" --weak-entropy --search-url http://{GATEWAY}:{net['http'].port}/echo?q=%s"
    dbg.spawn(argv)
    st = wait(dbg, lambda s: "page" in s and loaded(HOME)(s))
    dbg.settle(1.0)
    win = window(dbg)
    if not check("it opens on the start page, loaded", win and loaded(HOME)(st),
                 f"url={st.get('url')} loading={st.get('loading')} ext_h={st.get('ext_h')}"):
        return finish()
    page = st["page"]
    # NOT "taller than the view": NetSurf's root box is at least the
    # viewport, so the extent IS the view wherever the page fits -- the
    # start page does at the ASUS's 1366x768. Laid out is >= the view;
    # never laid out is 0.
    check("...laid out, its extent at least the view", st["ext_h"] >= page[3] > 0,
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
    photos(dbg, page, at, shot)
    fonts(dbg, page, at, shot)
    if net:
        network(dbg, net, page, at, shot)
    return finish()


def photos(dbg, page, at, shot):
    go(dbg, "/tests/web/photos.html")
    url = "file:///tests/web/photos.html"
    st = wait(dbg, loaded(url))
    check("a page of a JPEG and a PNG loads", st.get("url") == url, f"url={st.get('url')}")
    # The decode runs in a thread and the box fills on the redraw after
    # it: wait for the LAST region rather than sleeping.
    sx, sy = st.get("sx", 0), st.get("sy", 0)

    def box(x, y):   # a 40x30 box centred on page coordinates (x, y)
        return at((page[0] + x - sx - 20, page[1] + y - sy - 15, 40, 30))
    im = None
    for _ in range(20):
        im = shot("photos")
        if count(im, box(8 + 240, 256 + 210), near((90, 60, 30), 30)) > 900:
            break
        time.sleep(0.5)
    quads = [((8 + 80, 8 + 60), (220, 30, 30)), ((8 + 240, 8 + 60), (30, 180, 40)),
             ((8 + 80, 8 + 180), (30, 60, 220)), ((8 + 240, 8 + 180), (240, 210, 30))]
    got = [count(im, box(*xy), near(c, 30)) for xy, c in quads]
    check("the progressive JPEG is drawn, each quadrant its colour", min(got) > 900, f"{got} of 1200 px")
    bands = [((8 + 160, 256 + 30), (200, 40, 160)), ((8 + 160, 256 + 90), (20, 160, 160)),
             ((8 + 160, 256 + 150), (250, 140, 0)), ((8 + 160, 256 + 210), (90, 60, 30))]
    got = [count(im, box(*xy), near(c, 30)) for xy, c in bands]
    check("the 2-bit Adam7 PNG is drawn, each band its colour", min(got) > 900, f"{got} of 1200 px")
    white = count(im, box(8 + 320 + 60, 8 + 60), lambda p: min(p) > 245)
    check("...and the page beside them stays white (control)", white > 1100, f"{white} px")


# /tests/web/fonts.html: one 48 px row per style, the same word in each.
FONT_ROWS = ["sans", "sans again", "serif", "sans italic", "serif italic",
             "mono", "mono italic", "Times New Roman", "serif bold", "serif bold italic"]


def fonts(dbg, page, at, shot):
    go(dbg, "/tests/web/fonts.html")
    url = "file:///tests/web/fonts.html"
    st = wait(dbg, loaded(url))
    check("a page of serif, italic and monospace text loads", st.get("url") == url,
          f"url={st.get('url')}")
    im = shot("fonts")
    w = min(page[2] - 8, 420)

    def ink(row):    # the row's dark pixels, as a set of (x, y) within it
        x, y, _, h = at((page[0], page[1] + row * 48 - st.get("sy", 0), w, 48))
        crop = im.crop((x, y, x + w, y + h)).convert("L")
        return {(i % w, i // w) for i, v in enumerate(crop.get_flattened_data()) if v < 128}
    rows = {name: ink(i) for i, name in enumerate(FONT_ROWS)}

    def diff(a, b):
        return len(rows[a] ^ rows[b])
    check("every row is drawn", min(len(r) for r in rows.values()) > 300,
          f"{[len(r) for r in rows.values()]} ink px")
    check("...the same style twice draws the same ink (control)", diff("sans", "sans again") < 20,
          f"{diff('sans', 'sans again')} px differ")
    for a, b, what in [("sans", "serif", "serif is not sans"),
                       ("sans", "sans italic", "sans italic is slanted"),
                       ("serif", "serif italic", "serif italic is slanted"),
                       ("mono", "mono italic", "monospace italic is slanted"),
                       ("serif", "serif bold", "serif bold is bold"),
                       ("serif bold", "serif bold italic", "serif bold italic is slanted")]:
        check(f"{what}: its ink differs", diff(a, b) > 400, f"{diff(a, b)} px differ")
    check('"Times New Roman" draws in the serif face', diff("serif", "Times New Roman") < 20,
          f"{diff('serif', 'Times New Roman')} px differ")


def labels(dbg):
    return [t.get("label") for t in (window(dbg) or {}).get("tabs", [])]


def network(dbg, net, page, at, shot):
    srv = net["http"]
    srv.reset()
    host = f"{GATEWAY}:{srv.port}"
    go(dbg, f"{host}/net.html")             # no scheme: a local address is http
    url = f"http://{host}/net.html"
    st = wait(dbg, loaded(url), timeout=30)
    check("a typed host:port is fetched over http", st.get("url") == url and
          st.get("loading") == 0, f"url={st.get('url')} loading={st.get('loading')}")
    check("...titled from the page", "Fetched resources" in labels(dbg), f"tabs {labels(dbg)}")
    check("...and laid out whole (a long page)", st.get("ext_h", 0) > 3000,
          f"extent {st.get('ext_h')}")
    im = shot("net")
    red = count(im, at((page[0], page[1], page[2], 80)), near(NET_RED, 40))
    ctl = count(im, at((page[0], page[1] + 150, page[2], 120)), near(NET_RED, 40))
    check("...with its stylesheet, a SEPARATE request, applied", red > 100, f"{red} red px")
    check("...and none of its red lower down (control)", ctl < 20, f"{ctl} px")

    go(dbg, f"{host}/cookie/set?toy=yes")
    wait(dbg, loaded(f"http://{host}/cookie/set?toy=yes"), timeout=20)
    go(dbg, f"{host}/cookie/show")
    wait(dbg, loaded(f"http://{host}/cookie/show"), timeout=20)
    dbg.settle(1.0)
    check("a cookie the server set is sent back", "cookie toy=yes" in labels(dbg),
          f"tabs {labels(dbg)}")

    go(dbg, "toy os")
    want = f"http://{host}/echo?q=toy+os"
    st = wait(dbg, lambda s: s.get("url") == want, timeout=20)
    check("words that are not an address are searched for", st.get("url") == want,
          f"url={st.get('url')}")

    # An address longer than a toolkit field's default 127 bytes, typed.
    tail = "x" * 200
    go(dbg, f"{host}/net.html?q={tail}")
    want = f"http://{host}/net.html?q={tail}"
    whole = lambda s: (s.get("urllen") == len(want) and s.get("url")
                       and want.startswith(s["url"]) and s.get("loading") == 0)
    st = wait(dbg, whole, timeout=30)
    check("a typed address of over 200 bytes is followed whole", whole(st),
          f"{st.get('urllen')} bytes, want {len(want)}")

    conns, reqs = srv.counts()
    check("the server saw fewer connections than requests (reused)",
          reqs >= 7 and conns < reqs, f"connections={conns} requests={reqs}")

    if net.get("certs"):
        dbg.send(f"gui spawn /bin/wget -O /etc/ssl/certs/toyos-test-ca.pem http://{host}/ca.pem")
        for _ in range(40):
            if "BEGIN CERTIFICATE" in (dbg.send("sh cat /etc/ssl/certs/toyos-test-ca.pem") or ""):
                break
            time.sleep(0.3)
        # A new process: the trust store is parsed once per process.
        good = f"https://{GATEWAY}:{net['good'].port}/table.html"
        dbg.send("gui key 0x14 ctrl")      # Ctrl+T, so the tab count is the check's own
        # The new tab must EXIST before the address is typed: keys sent
        # into the gap land in the old tab, or nowhere (seen under the
        # full suite's load).
        wait(dbg, lambda s: s.get("tabs") == 2 and loaded(HOME)(s), timeout=20)
        go(dbg, good)
        st = wait(dbg, loaded(good), timeout=40)
        check("https with a trusted test CA loads", st.get("url") == good and "Tables" in labels(dbg),
              f"url={st.get('url')} tabs {labels(dbg)}")
        bad = f"https://{GATEWAY}:{net['expired'].port}/table.html"
        go(dbg, bad)
        # NetSurf's certificate page keeps the address it refused, and
        # names itself; the page's own title must NOT appear.
        for _ in range(80):
            if "Privacy error" in labels(dbg):
                break
            time.sleep(0.5)
        st = state(dbg)
        check("an expired certificate stops at the certificate page",
              "Privacy error" in labels(dbg) and labels(dbg).count("Tables") == 0,
              f"tabs {labels(dbg)} url={st.get('url')} loading={st.get('loading')}")

    win = window(dbg)
    dbg.send(f"gui close {win['z']}")
    gone = False
    for _ in range(40):
        if not window(dbg):
            gone = True
            break
        time.sleep(0.25)
    jar = dbg.send("sh cat /var/lib/browser/cookies") or ""
    check("closing the window keeps the cookie in /var/lib/browser",
          gone and "toy" in jar and "yes" in jar, jar.strip()[-120:])


def finish():
    return _res.finish("browser_test")


if __name__ == "__main__":
    sys.exit(main())
