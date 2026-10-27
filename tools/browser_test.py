#!/usr/bin/env python3
"""The Web Browser: NetSurf's pages drawn in a SaunaOS window, and driven.

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
  - the ☰ menu opens and reports its rows; its Zoom in lays the page out
    taller at 110%, and Ctrl+0 returns it to the SAME extent;
  - Ctrl+F finds "line" in /tests/web/text.html 1 of 8 times, with the
    page's pixels changed by the highlights; Enter steps on, Shift+Enter
    wraps back past the first to the last, and Esc leaves the page
    pixel-identical to before the search (the round trip);
  - one popup at a time: a click on the page closes the Downloads flyout
    (a popup that is a CONTAINER, which the router used to gate), opening
    the menu closes the flyout, and a click outside closes the menu;
  - the pointer is an I-beam over a page's text and the arrow over the
    blank page beside it -- NetSurf itself leaves text to the arrow;
  - Ctrl+D bookmarks a page and fills the address field's star; Ctrl+B
    opens the sidebar on it and narrows the page, its row goes to the
    page, Ctrl+H lists History under a day's heading, its search narrows
    it to one entry, Backspace edits that search (Ctrl+H and Backspace
    share a code), the row's remove button deletes the bookmark, and
    Ctrl+B again closes the sidebar; the bookmark is in its file after
    the window closes;
  - downloads (over the network): an attachment lands in /home/Downloads
    with the server's exact bytes (CRC-32 from `sum`), a second of the
    same name is "name (1)", a broken connection and a Cancel both leave
    nothing behind, and Clear finished empties the flyout;
  - /tests/web/fonts.html draws serif, italic and monospace italic each
    in its own face -- the same word's ink differs from the upright or
    sans row beside it -- while a row repeated, and "Times New Roman"
    against `serif`, match (the controls: a broken version that drew
    every row the same would pass "it differs" nowhere);
  - a page's content process killed: the app survives, the C3 view is
    up (the last frame greyed under a danger-tinted bar), Reload brings
    the page back in a new process; about:crash leaves a crash report
    that View report opens (stage 5);
  - /tests/web/svg.html: an SVG ring is drawn with its inner contour a
    HOLE (nonzero), two same-way squares fill their overlap (not
    even-odd), a curved stroke is drawn, and a checkbox at opacity 0 over
    a green label leaves the label green while one at full opacity draws
    (control) and a plain box at opacity 0 still draws;
  - /tests/web/overflow.html: a zero-height box with "overflow: hidden
    auto" holding 300 items leaves the page no taller than the view --
    the two-value shorthand libcss dropped, which gave Wikipedia's Main
    Page 12,000 px of blank below its footer;
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
  - a page of 1 MiB -- more than the network ring holds -- loads whole;
  - a cookie the server set is sent back on the next page, and kept in
    /var/lib/browser/cookie-jar (the network process's) when the window
    closes;
  - words that are not an address become a search (--search-url points
    it at the same server, so nothing leaves the machine);
  - the server saw FEWER connections than requests: they were reused;
  - https with a test CA (fetched into /etc/ssl/certs by wget) loads, and
    an expired certificate stops at NetSurf's "Privacy error" page;
  - the page's engine runs SANDBOXED (`ps --sandbox` shows its four
    promises), an http page's file: image is refused by the app while
    the same image over http draws, and the engine is refused nothing
    from /etc at start.

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
from web_server import WebServer, make_ca, GATEWAY, download_bytes  # noqa: E402
import zlib  # noqa: E402

APP = "/bin/wm/apps/browser"
HOME = "file:///usr/share/netsurf/start.html"
HEADER_BLUE = (50, 90, 160)        # start.html's .top background, #325aa0
HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "-": "0x2d", "_": "0x5f", ":": "0x3a",
       "?": "0x3f", "=": "0x3d"}
NET_RED = (192, 48, 48)            # net.css's h1 border, #c03030
# enum ns_site_state (userland/browser/ns_site.h)
SITE_FILE, SITE_INSECURE, SITE_SECURE = 2, 3, 6
DANGER = (178, 58, 50)             # utheme's DANGER action role
ACCENT = (70, 110, 160)            # utheme_default()'s accent
# The shape the compositor RESOLVED (cursor_shape()), in wm_internal.h's
# WM_CURSOR_* numbering -- not the client's WIN_CURSOR_*.
CURSOR_DEFAULT, CURSOR_TEXT = 0, 4
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
        m = re.search(r"browser: layout menu\.item 0 (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _st.setdefault("menu_items", {})[int(m.group(1))] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"browser: site (\d+) zoom (\d+)(?: scripts (\d+) (\d+))?$", line)
        if m:
            _st["site"] = int(m.group(1))
            if m.group(3) is not None:
                _st["js"], _st["scripts"] = int(m.group(3)), int(m.group(4))
        m = re.search(r"browser: card (\d+) cookies (\d+) chain (\d+) tls (\S+) logins (\d+)$", line)
        if m:
            _st["cardinfo"] = (int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4))
            _st["card_logins"] = int(m.group(5))
        m = re.search(r"browser: cookies cleared (\d+)$", line)
        if m:
            _st["cleared"] = int(m.group(1))
        # The card's JavaScript row: a segment per value (USITEPERM_*).
        m = re.search(r"browser: layout card\.js\.slot (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _st.setdefault("js_slot", {})[int(m.group(1))] = tuple(int(v) for v in m.groups()[1:])
        # The page's links in view, a fresh set from each link 0.
        m = re.search(r"browser: layout page\.link (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            if m.group(1) == "0":
                _st["links"], _st["hrefs"] = {}, {}
            _st.setdefault("links", {})[int(m.group(1))] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"browser: layout page\.href\.(\d+) (\S+)$", line)
        if m:
            _st.setdefault("hrefs", {})[int(m.group(1))] = m.group(2)
        # A widget's scalar ("layout page.pid 123", "page.stopped 1").
        m = re.search(r"browser: layout (\S+) (-?\d+)$", line)
        if m:
            _st[m.group(1)] = int(m.group(2))
        m = re.search(r"browser: layout card\.open (\d)$", line)
        if m:
            _st["card_open"] = int(m.group(1))
        m = re.search(r"browser: download (start|done|failed|cancelled) (.+) (\d+)(: .*)?$", line)
        if m:
            _st.setdefault("dl", []).append((m.group(1), m.group(2), int(m.group(3))))
        m = re.search(r"browser: layout downloads\.action (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _st.setdefault("dl_action", {})[int(m.group(1))] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"browser: layout downloads\.count (\d+)$", line)
        if m:
            _st["dl_count"] = int(m.group(1))
        m = re.search(r"browser: layout menu\.open (\d)$", line)
        if m:
            _st["menu_open"] = int(m.group(1))
        m = re.search(r"browser: layout dlpanel\.open (\d)$", line)
        if m:
            _st["dlp_open"] = int(m.group(1))
        m = re.search(r"browser: bookmark (\d)$", line)
        if m:
            _st["bookmarked"] = int(m.group(1))
        m = re.search(r"browser: sidebar (\d) rows (\d+)$", line)
        if m:
            _st["sidebar"] = (int(m.group(1)), int(m.group(2)))
        m = re.search(r"browser: layout side-list\.(row|remove) (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _st.setdefault("side_" + m.group(1), {})[int(m.group(2))] = tuple(int(v) for v in m.groups()[2:])
        m = re.search(r"browser: signin open (\S+) retry (\d) secure (\d)$", line)
        if m:
            # "login", not "signin": the form's own layout line is "layout signin".
            _st["login"] = (m.group(1), int(m.group(2)), int(m.group(3)))
            _st["logins"] = _st.get("logins", 0) + 1
        m = re.search(r"browser: signin (given|cancelled)$", line)
        if m:
            _st["login_done"] = m.group(1)
        m = re.search(r"browser: https fallback (\S+)$", line)
        if m:
            _st["fallback"] = m.group(1)
            _st["fallbacks"] = _st.get("fallbacks", 0) + 1
        m = re.search(r"browser: zoom (\d+)$", line)
        if m:
            _st["zoom"] = int(m.group(1))
        m = re.search(r"browser: find (\d+) of (\d+)$", line)
        if m:
            _st["found"] = (int(m.group(1)), int(m.group(2)))
        m = re.search(r"browser: tab (\d+)/(\d+) loading (\d) extent (\d+) (\d+) "
                      r"scroll (-?\d+) (-?\d+) len (\d+) loads (\d+) url (\S*)", line)
        if m:
            g = m.groups()
            # The app reports at most 72 bytes of the address: `url` is
            # then a prefix, and `urllen` its real length.
            _st.update(cur=int(g[0]), tabs=int(g[1]), loading=int(g[2]), ext_w=int(g[3]),
                       ext_h=int(g[4]), sx=int(g[5]), sy=int(g[6]), urllen=int(g[7]), loads=int(g[8]), url=g[9])
    return _st


def wait(dbg, ok, timeout=12.0):
    deadline = time.time() + timeout
    st = state(dbg)
    while not ok(st) and time.time() < deadline:
        time.sleep(0.2)
        st = state(dbg)
    return st


def window(dbg):
    # By app_id: a long page title cuts "Web Browser" off the window's.
    wins = [w for w in dbg.windows() if w.get("app_id") == "browser" or w["title"].endswith("Web Browser")]
    return wins[-1] if wins else None


def type_text(dbg, text):
    # A pause every 30 keys, for the debug console's sake: the compositor
    # holds what the app's inbox cannot take (input_backlog_test.py), so
    # a burst no longer loses keys.
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
    if not os.environ.get("SAUNAOS_REMOTE_HOST"):
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


SESSION = "/var/lib/browser/session.tsv"


def forget_session(dbg):
    """A launch restores the tabs of the last one (browser/ns_session.c):
    a check that wants the start page starts without them."""
    dbg.send(f"sh rm {SESSION}")


def run(dbg, qmp, args, net):
    old = window(dbg)
    if old:
        dbg.send(f"gui close {old['z']}")
        dbg.settle(1.0)
    forget_session(dbg)
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

    # The start page is the engine's own file and its test pages are not:
    # a click between them is the page's navigation, not the app's.
    text = "file:///tests/web/text.html"
    st = wait(dbg, lambda s: text in s.get("hrefs", {}).values())
    i = next((k for k, h in st.get("hrefs", {}).items() if h == text), None)
    r = st.get("links", {}).get(i)
    if check("the start page reports its Text card's link", r is not None, f"hrefs {st.get('hrefs')}"):
        dbg.send("gui click %d %d" % (c["x"] + r[0] + r[2] // 2, c["y"] + r[1] + r[3] // 2))
        st = wait(dbg, loaded(text))
        check("...and a click on it loads the test page", st.get("url") == text,
              f"url={st.get('url')} loading={st.get('loading')}")
        im = shot("start_click")
        ink = count(im, at(page), lambda p: max(p) < 90)
        tabs = [t.get("label") for t in (window(dbg) or {}).get("tabs", [])]
        check("...drawn, not an error page", "Text" in " ".join(map(str, tabs)) and ink > 300,
              f"tabs {tabs} ink {ink}")
        dbg.send("gui key 0xF784 alt")     # Alt+Left
        wait(dbg, loaded(HOME))

    # Focused, the bar's border is 2 px, and the field inside it must not
    # paint its ground over the inner row (it once did, along its width).
    dbg.send("gui key 0x0c ctrl")
    type_text(dbg, "abc")
    st = state(dbg)
    bar, fld = st.get("addressbar"), st.get("address")
    if check("the address bar and its field report their rects", bar and fld, f"{bar} {fld}"):
        im = shot("addr_focus")
        fx = c["x"] + fld[0] + fld[2] // 2
        outer, inner = im.getpixel((fx, c["y"] + bar[1])), im.getpixel((fx, c["y"] + bar[1] + 1))
        bot = im.getpixel((fx, c["y"] + bar[1] + bar[3] - 2))
        ground = im.getpixel((fx, c["y"] + bar[1] + 3))
        check("focused, the bar's 2 px border is whole across the field",
              near(outer, 8)(inner) and near(outer, 8)(bot) and min(outer) < 200,
              f"outer {outer} inner {inner} bottom {bot}")
        check("...and the field's ground inside it is white (control)", min(ground) > 245, f"{ground}")

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
    st = state(dbg)
    chip = st.get("addressbar.chip")
    check("the address field says a local page is a File", st.get("site") == SITE_FILE and chip,
          f"site {st.get('site')} chip {chip}")
    if chip:
        ink = count(im, at(chip), lambda p: max(p) < 150)
        check("...in a chip that is drawn", ink > 20, f"{ink} dark px")

    photos(dbg, page, at, shot)
    svg_and_opacity(dbg, page, at, shot)
    overflow_two_values(dbg, page)
    fonts(dbg, page, at, shot)
    text_cursor(dbg, qmp, page, at)
    zoom_and_find(dbg, page, at, shot, c)
    popups(dbg, page, at, c)
    bookmarks_and_history(dbg, page, at, shot)
    stopped_page(dbg, page, at, shot)
    javascript_local(dbg, page, at, shot)
    out_of_memory(dbg)
    if net:
        network(dbg, net, page, at, shot)
    history_survives(dbg, page)
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
    check("the progressive JPEG is drawn, each quadrant its colour", min(got) > 900,
          f"{got} of 1200 px, scrolled {sx},{sy}")
    bands = [((8 + 160, 256 + 30), (200, 40, 160)), ((8 + 160, 256 + 90), (20, 160, 160)),
             ((8 + 160, 256 + 150), (250, 140, 0)), ((8 + 160, 256 + 210), (90, 60, 30))]
    got = [count(im, box(*xy), near(c, 30)) for xy, c in bands]
    check("the 2-bit Adam7 PNG is drawn, each band its colour", min(got) > 900, f"{got} of 1200 px")
    white = count(im, box(8 + 320 + 60, 8 + 60), lambda p: min(p) > 245)
    check("...and the page beside them stays white (control)", white > 1100, f"{white} px")


def overflow_two_values(dbg, page):
    go(dbg, "/tests/web/overflow.html")
    url = "file:///tests/web/overflow.html"
    st = wait(dbg, loaded(url))
    # 300 list items are ~6000 px; clipped by "overflow: hidden auto" in
    # a zero-height box they add nothing, so the page is the view.
    check("two-value overflow clips a hidden list: the page does not scroll",
          st.get("url") == url and 0 < st.get("ext_h", 0) <= page[3] + 40,
          f"extent {st.get('ext_h')} for a view {page[3]} high")


def svg_and_opacity(dbg, page, at, shot):
    go(dbg, "/tests/web/svg.html")
    url = "file:///tests/web/svg.html"
    st = wait(dbg, loaded(url))
    check("a page of SVG images and opacity-0 boxes loads", st.get("url") == url,
          f"url={st.get('url')}")
    sx, sy = st.get("sx", 0), st.get("sy", 0)

    def box(x, y, w=8, h=8):   # page coordinates, centred on (x, y)
        return at((page[0] + x - sx - w // 2, page[1] + y - sy - h // 2, w, h))
    red, blue, green = near((200, 40, 40), 30), near((30, 60, 220), 30), near((20, 160, 60), 30)
    white = lambda p: min(p) > 245     # noqa: E731
    im = None
    for _ in range(20):                # the SVGs are fetched after the page
        im = shot("svg")
        if count(im, box(88 + 52, 88), red) > 50:
            break
        time.sleep(0.5)
    # ring.svg at (8, 8) 160 px: a band from r 32 to r 72 about (88, 88).
    got = [count(im, box(88 + 52, 88), red), count(im, box(88, 88 - 52), red)]
    check("an SVG path is drawn: ring.svg's band is red", min(got) > 56, f"{got} of 64 px")
    hole = count(im, box(88, 88, 16, 16), white)
    check("...with a HOLE where its inner contour runs the other way (nonzero)", hole > 240,
          f"{hole} of 256 px white")
    out = count(im, box(14, 14), white)
    check("...and nothing outside the circle (control)", out > 60, f"{out} of 64 px white")
    # overlap.svg at (184, 8), scale 1.6: squares 5-55 and 30-80 in one path.
    over = count(im, box(184 + 68, 8 + 68), blue)
    one = count(im, box(184 + 24, 8 + 24), blue)
    check("two squares wound the same way fill their OVERLAP (nonzero, not even-odd)",
          over > 56 and one > 56, f"overlap {over}, one square {one} of 64 px")
    stroke = count(im, box(184 + 80, 8 + 121, 6, 6), green)
    check("a curved stroke 8 units wide is drawn along its curve", stroke > 30, f"{stroke} of 36 px")
    # A checkbox at opacity 0 over a green label: the label all green.
    label = count(im, box(68, 220, 120, 40), near((60, 180, 60), 30))
    check("a checkbox at opacity 0 is NOT drawn over its label", label > 4700,
          f"{label} of 4800 px green")
    shown = count(im, box(204, 220, 40, 40), lambda p: max(p) < 200)
    check("...one at full opacity is (control)", shown > 40, f"{shown} dark px")
    faded = count(im, box(420, 220, 120, 40), blue)
    check("...and a plain box at opacity 0 still draws (no script will fade it in)", faded > 4700,
          f"{faded} of 4800 px blue")


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


def text_cursor(dbg, qmp, page, at):
    """Over text the I-beam; over the blank page beside it, the arrow."""
    # fonts.html is loaded: row 0's word runs from x=8 to ~300 at y 0..48.
    word = at((page[0] + 60, page[1] + 24, 0, 0))[:2]
    blank = at((page[0] + page[2] - 80, page[1] + 24, 0, 0))[:2]
    dbg.warp_cursor(qmp, *word)
    dbg.settle(0.5)
    over = dbg.cursor_shape()
    dbg.warp_cursor(qmp, *blank)
    dbg.settle(0.5)
    beside = dbg.cursor_shape()
    check("the pointer is an I-beam over a page's text", over == CURSOR_TEXT, f"shape {over}")
    check("...and the arrow over the blank page beside it (control)", beside == CURSOR_DEFAULT,
          f"shape {beside}")


# The ☰ menu's rows, in browser.c's open_menu() order.
MENU_ZOOM_IN, MENU_FIND = 2, 6


def zoom_and_find(dbg, page, at, shot, c):
    url = "file:///tests/web/text.html"
    go(dbg, "/tests/web/text.html")
    st = wait(dbg, loaded(url))
    base = dict(st)
    base_im = shot("find_before")

    # Zoom from the ☰ menu, by the row the menu itself reports.
    _st.pop("menu_items", None)
    tx, ty, tw, th = st["tools-right"]          # Downloads, then ☰: its right half
    dbg.send("gui click %d %d" % (c["x"] + tx + tw * 3 // 4, c["y"] + ty + th // 2))
    dbg.settle(0.5)
    rows = wait(dbg, lambda s: MENU_ZOOM_IN in s.get("menu_items", {}), 4).get("menu_items", {})
    check("the ☰ menu opens, with its rows", MENU_ZOOM_IN in rows and MENU_FIND in rows,
          f"rows {sorted(rows)}")
    if MENU_ZOOM_IN in rows:
        x, y, w, h = rows[MENU_ZOOM_IN]
        dbg.send("gui click %d %d" % (c["x"] + x + w // 2, c["y"] + y + h // 2))
    st = wait(dbg, lambda s: s.get("zoom") == 110 and s.get("ext_h", 0) > base["ext_h"], 6)
    check("its Zoom in scales the page to 110%, and it lays out taller",
          st.get("zoom") == 110 and st.get("ext_h", 0) > base["ext_h"],
          f"zoom {st.get('zoom')} extent {base['ext_h']} -> {st.get('ext_h')}")
    dbg.send("gui key 0x30 ctrl")      # Ctrl+0
    st = wait(dbg, lambda s: s.get("zoom") == 100 and s.get("ext_h") == base["ext_h"], 6)
    check("...and Ctrl+0 puts it back, to the same extent (round trip)",
          st.get("zoom") == 100 and st.get("ext_h") == base["ext_h"],
          f"zoom {st.get('zoom')} extent {st.get('ext_h')} vs {base['ext_h']}")

    _st.pop("addressbar.zoom", None)
    dbg.send("gui key 0x3d ctrl")      # Ctrl+=
    st = wait(dbg, lambda s: s.get("zoom") == 110 and "addressbar.zoom" in s, 6)
    badge = st.get("addressbar.zoom")
    check("a zoomed page shows its level in the address field", badge is not None,
          f"zoom {st.get('zoom')} badge {badge}")
    if badge:
        x, y, w, h = badge
        dbg.send("gui click %d %d" % (c["x"] + x + w // 2, c["y"] + y + h // 2))
    st = wait(dbg, lambda s: s.get("zoom") == 100 and s.get("ext_h") == base["ext_h"], 6)
    check("...and a click on that badge resets it", st.get("zoom") == 100, f"zoom {st.get('zoom')}")

    # Find: "line" is in text.html eight times, ignoring case -- "Line 1" to
    # "Line 6", "lines", and the heading's "inline".
    dbg.send("gui key 0x06 ctrl")      # Ctrl+F
    dbg.settle(0.5)
    type_text(dbg, "line")
    st = wait(dbg, lambda s: s.get("found") == (1, 8), 6)
    check("Ctrl+F and a query find every match: 1 of 8", st.get("found") == (1, 8),
          f"found {st.get('found')}")
    im = shot("find_on")
    pg = at((page[0], page[1], page[2] - 16, page[3]))
    x, y, w, h = pg
    a = im.crop((x, y, x + w, y + h)).get_flattened_data()
    b = base_im.crop((x, y, x + w, y + h)).get_flattened_data()
    lit = sum(1 for p, q in zip(a, b) if p != q)
    check("...and the matches are highlighted on the page", lit > 300, f"{lit} px changed")
    dbg.send("gui key 0x0d")
    st = wait(dbg, lambda s: s.get("found") == (2, 8), 4)
    check("Enter steps to the next match", st.get("found") == (2, 8), f"found {st.get('found')}")
    for _ in range(2):
        dbg.send("gui key 0x0d shift")
    st = wait(dbg, lambda s: s.get("found") == (8, 8), 6)
    check("Shift+Enter steps back, wrapping past the first to the last",
          st.get("found") == (8, 8), f"found {st.get('found')}")
    dbg.send("gui key 0x1b")
    dbg.settle(0.5)
    dbg.send("gui key 0xf786")         # Home: the last match scrolled the page
    wait(dbg, lambda s: s.get("sy") == base.get("sy"), 4)
    im = shot("find_off")
    a = im.crop((x, y, x + w, y + h)).get_flattened_data()
    diff = sum(1 for p, q in zip(a, b) if p != q)
    check("Esc closes it, and the page is as it was before (round trip)", diff < 20,
          f"{diff} px differ from before the search")


def click_rect(dbg, at, r):
    x, y, w, h = r
    dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))


def popups(dbg, page, at, c):
    """One popup at a time, and each closed by a click outside it."""
    blank = at((page[0] + page[2] - 60, page[1] + page[3] // 2, 0, 0))[:2]
    dbg.send("gui key 0x0a ctrl")      # Ctrl+J
    st = wait(dbg, lambda s: s.get("dlp_open") == 1, 4)
    dbg.send("gui click %d %d" % blank)
    st = wait(dbg, lambda s: s.get("dlp_open") == 0, 4)
    check("a click on the page closes the Downloads flyout", st.get("dlp_open") == 0,
          f"open {st.get('dlp_open')}")
    dbg.send("gui key 0x0a ctrl")
    wait(dbg, lambda s: s.get("dlp_open") == 1, 4)
    tx, ty, tw, th = state(dbg)["tools-right"]
    dbg.send("gui click %d %d" % (c["x"] + tx + tw * 3 // 4, c["y"] + ty + th // 2))
    st = wait(dbg, lambda s: s.get("menu_open") == 1 and s.get("dlp_open") == 0, 4)
    check("opening the menu closes the flyout: one popup at a time",
          st.get("menu_open") == 1 and st.get("dlp_open") == 0,
          f"menu {st.get('menu_open')} flyout {st.get('dlp_open')}")
    dbg.send("gui click %d %d" % blank)
    st = wait(dbg, lambda s: s.get("menu_open") == 0, 4)
    check("...and a click on the page closes the menu", st.get("menu_open") == 0,
          f"menu {st.get('menu_open')}")


def bookmarks_and_history(dbg, page, at, shot):
    text = "file:///tests/web/text.html"
    go(dbg, "/tests/web/text.html")
    wait(dbg, loaded(text))
    dbg.send("gui key 0x04 ctrl")      # Ctrl+D
    st = wait(dbg, lambda s: s.get("bookmarked") == 1 and "addressbar.star" in s, 6)
    im = shot("starred")
    star = st.get("addressbar.star")
    # The ACCENT, close: an unfilled star's outline is the muted grey,
    # which a looser match takes for blue.
    blue = count(im, at(star), near(ACCENT, 25)) if star else 0
    check("Ctrl+D bookmarks the page, and the address field's star fills",
          st.get("bookmarked") == 1 and blue > 10, f"bookmarked {st.get('bookmarked')} {blue} accent px")

    _st.pop("side_row", None)
    dbg.send("gui key 0x02 ctrl")      # Ctrl+B
    st = wait(dbg, lambda s: s.get("sidebar") == (0, 1) and s.get("side_row"), 6)
    check("Ctrl+B opens the sidebar on Bookmarks, holding that page", st.get("sidebar") == (0, 1),
          f"sidebar {st.get('sidebar')}")
    st2 = state(dbg)
    check("...beside the page, which narrows for it", st2.get("page", page)[0] > page[0],
          f"page {st2.get('page')} was {page}")
    dbg.send("gui key 0xf786 alt")     # Alt+Home: away, so the click has somewhere to go
    wait(dbg, loaded(HOME))
    row = st.get("side_row", {}).get(0)
    if row:
        click_rect(dbg, at, row)
    st = wait(dbg, loaded(text), 8)
    check("a click on the bookmark goes to it", st.get("url") == text, f"url {st.get('url')}")

    dbg.send("gui key 0x08 ctrl")      # Ctrl+H -- not Backspace: the ctrl bit
    st = wait(dbg, lambda s: s.get("sidebar", (0, 0))[0] == 1 and s["sidebar"][1] >= 4, 6)
    check("Ctrl+H switches it to History: the pages visited, under a day's heading",
          st.get("sidebar", (0, 0))[0] == 1 and st["sidebar"][1] >= 4, f"sidebar {st.get('sidebar')}")
    search = state(dbg).get("side-search")
    if search:
        click_rect(dbg, at, search)
    type_text(dbg, "fonts")
    st = wait(dbg, lambda s: s.get("sidebar") == (1, 2), 6)
    check("...and its search narrows it to the one page that matches (a heading and an entry)",
          st.get("sidebar") == (1, 2), f"sidebar {st.get('sidebar')}")
    for _ in range(5):
        dbg.send("gui key 0x08")       # Backspace: the query emptied, NOT History toggled
    st = wait(dbg, lambda s: s.get("sidebar", (0, 0))[1] >= 4, 6)
    check("...Backspace edits the search rather than toggling History",
          st.get("sidebar", (0, 0))[0] == 1 and st["sidebar"][1] >= 4, f"sidebar {st.get('sidebar')}")

    _st.pop("side_remove", None)
    dbg.send("gui key 0x02 ctrl")      # Ctrl+B: back to Bookmarks
    st = wait(dbg, lambda s: s.get("sidebar") == (0, 1) and 0 in s.get("side_remove", {}), 6)
    rm = st.get("side_remove", {}).get(0)
    if rm:
        click_rect(dbg, at, rm)
    st = wait(dbg, lambda s: s.get("sidebar") == (0, 0), 6)
    st = wait(dbg, lambda s: s.get("bookmarked") == 0 or s.get("sidebar") == (0, 0), 2)
    check("the row's remove button deletes the bookmark", st.get("sidebar") == (0, 0),
          f"sidebar {st.get('sidebar')}")
    dbg.send("gui key 0x04 ctrl")      # Ctrl+D again: kept for the jar check at the end
    wait(dbg, lambda s: s.get("bookmarked") == 1, 6)
    dbg.send("gui key 0x02 ctrl")      # Ctrl+B on Bookmarks: closes it
    st = wait(dbg, lambda s: s.get("page", (0,))[0] == page[0], 6)
    check("...and Ctrl+B again closes the sidebar, the page wide again",
          st.get("page", (0,))[0] == page[0], f"page {st.get('page')}")


def saturated(p):
    return max(p) - min(p) > 60


def stopped_page(dbg, page, at, shot):
    """The engine as a process (docs/browser-design.md, stage 5): a page's
    content process KILLED leaves the app running and the C3 view up --
    the last frame greyed under a danger-tinted bar -- and Reload brings
    the page back in a NEW process. about:crash then faults the engine on
    purpose, which leaves a crash report the bar's View report opens.
    What a broken version still passes: a bar drawn over a page that is
    still live (the frame would keep its colour), or a Reload that
    reuses a dead pid (it would never load)."""
    url = "file:///tests/web/photos.html"
    go(dbg, "/tests/web/photos.html")
    st = wait(dbg, lambda s: loaded(url)(s) and s.get("page.pid", 0) > 0)
    pid = st.get("page.pid", 0)
    rect = at((page[0], page[1], page[2] - 16, page[3]))
    im = shot("before_kill")
    colour = count(im, rect, saturated)
    check("a page in colour, drawn by its own content process (control)", pid > 0 and colour > 5000,
          f"pid {pid}, {colour} saturated px")

    dbg.send(f"sh kill -KILL {pid}")
    st = wait(dbg, lambda s: s.get("page.stopped") == 1, 10)
    win = window(dbg)
    check("killing the content process stops the page, and the app is still there",
          st.get("page.stopped") == 1 and win is not None,
          f"stopped {st.get('page.stopped')} window {bool(win)}")
    im = shot("stopped")
    reload_ = st.get("page.reload")
    bar = st.get("page.bar")
    left = count(im, rect, saturated)
    below = at((page[0], page[1] + (bar[3] if bar else 40), page[2] - 16, page[3] - (bar[3] if bar else 40)))
    kept = count(im, below, lambda p: min(p) < 235)
    check("...its last frame kept, greyed: no colour left in it, but not blank",
          kept > 2000 and left < colour // 20, f"{left} saturated px of {colour} before, {kept} drawn")
    tint = count(im, at(bar), lambda p: p[0] > p[1] + 15 and p[0] > p[2] + 15) if bar else 0
    check("...under the bar, in the danger colour's tint", bar and tint > 500, f"bar {bar}, {tint} px")
    check("...whose View report is unavailable: a killed process left no report",
          st.get("page.report") is None, f"report {st.get('page.report')}")
    labels_now = window(dbg).get("tabs", []) if win else []
    check("...and the tab still shows the page's title", labels_now and labels_now[0].get("label"),
          f"tabs {labels_now}")

    if reload_:
        x, y, w, h = reload_
        dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
    st = wait(dbg, lambda s: s.get("page.stopped") == 0 and loaded(url)(s) and
              s.get("page.pid", 0) not in (0, pid), 15)
    im = shot("reloaded")
    again = count(im, rect, saturated)
    check("Reload brings the page back, in a new process, in colour",
          st.get("page.stopped") == 0 and st.get("page.pid", 0) not in (0, pid) and again > colour // 2,
          f"stopped {st.get('page.stopped')} pid {st.get('page.pid')} (was {pid}), {again} px")

    pid = st.get("page.pid", 0)
    go(dbg, "about:crash")
    st = wait(dbg, lambda s: s.get("page.stopped") == 1 and s.get("page.report"), 15)
    ls = dbg.send("sh ls /var/crash") or ""
    check("about:crash faults the engine: the page stops, and a crash report is written",
          st.get("page.stopped") == 1 and f"webcontent-{pid}.crash" in ls,
          f"stopped {st.get('page.stopped')} report {st.get('page.report')} ls {ls.strip()[-160:]}")
    rep = st.get("page.report")
    if rep:
        x, y, w, h = rep
        dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
    viewer = None
    for _ in range(40):
        # The viewer opened on one report is titled for it ("webcontent crashed").
        viewer = next((w for w in dbg.windows() if "webcontent crashed" in w["title"]), None)
        if viewer:
            break
        time.sleep(0.25)
    check("...and View report opens it in Crash Reports", viewer is not None,
          f"windows {[w['title'] for w in dbg.windows()]}")
    if viewer:
        dbg.send(f"gui close {viewer['z']}")
        dbg.settle(1.0)
    reload_ = st.get("page.reload")
    if reload_:
        x, y, w, h = reload_
        dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
    st = wait(dbg, lambda s: s.get("page.stopped") == 0 and loaded(url)(s), 15)
    check("...and Reload recovers that one too", st.get("page.stopped") == 0 and loaded(url)(st),
          f"stopped {st.get('page.stopped')} url {st.get('url')}")


def history_survives(dbg, page):
    """Back/forward is the APP's list (uui_webview's hist[]), so a page
    whose content process died still has it, each entry comes back
    scrolled where it was left, and the browser's next launch reopens
    its tabs with their lists (browser/ns_session.c). What a broken
    version still passes: NetSurf's own history would be lost with the
    killed process (Back would do nothing), a list without scroll
    positions comes back at the top, and no session opens the start
    page."""
    a, b, d = ("file:///tests/web/" + n for n in ("table.html", "text.html", "links.html"))
    win = window(dbg)
    c = win["content"]
    go(dbg, "/tests/web/table.html")
    wait(dbg, loaded(a))
    go(dbg, "/tests/web/text.html")
    wait(dbg, loaded(b))
    dbg.move(c["x"] + page[0] + page[2] // 2, c["y"] + page[1] + page[3] // 2)
    for _ in range(4):
        dbg.wheel(-1)
    st = wait(dbg, lambda s: s.get("sy", 0) > 0, timeout=4)
    dbg.settle(1.0)
    sy = state(dbg).get("sy", 0)
    go(dbg, "/tests/web/links.html")
    st = wait(dbg, lambda s: loaded(d)(s) and s.get("page.pid", 0) > 0)
    pid = st.get("page.pid", 0)
    check("history: three pages visited, the second scrolled (control)", sy > 0 and loaded(d)(st),
          f"scrolled {sy} url {st.get('url')} pid {pid}")

    dbg.send(f"sh kill -KILL {pid}")
    st = wait(dbg, lambda s: s.get("page.stopped") == 1, 10)
    dbg.send("gui key 0xF784 alt")     # Alt+Left
    st = wait(dbg, lambda s: s.get("page.stopped") == 0 and loaded(b)(s) and
              s.get("page.pid", 0) not in (0, pid) and s.get("sy", 0) > 0, 15)
    check("Back on a stopped page goes back, in a new process", loaded(b)(st) and
          st.get("page.pid", 0) not in (0, pid), f"url {st.get('url')} pid {st.get('page.pid')} (was {pid})")
    check("...to where that page was scrolled", st.get("sy") == sy, f"scroll {st.get('sy')}, left at {sy}")
    dbg.send("gui key 0xF784 alt")
    st = wait(dbg, lambda s: loaded(a)(s) and s.get("sy", -1) == 0)
    check("...and again, to the page before", loaded(a)(st), f"url {st.get('url')}")
    dbg.send("gui key 0xF785 alt")     # Alt+Right
    st = wait(dbg, lambda s: loaded(b)(s) and s.get("sy", 0) > 0)
    check("Forward returns, scrolled as it was", loaded(b)(st) and st.get("sy") == sy,
          f"url {st.get('url')} scroll {st.get('sy')}")

    # The session: closed with the middle page in front, Forward ahead of it.
    dbg.send(f"gui close {win['z']}")
    gone = False
    for _ in range(40):
        if not window(dbg):
            gone = True
            break
        time.sleep(0.25)
    saved = dbg.send(f"sh cat {SESSION}") or ""
    check("closing the window saves its tabs' lists", gone and "tests/web/links.html" in saved and
          "tests/web/table.html" in saved, saved.strip()[-200:])
    _st.clear()
    dbg.spawn(APP)
    st = wait(dbg, lambda s: loaded(b)(s) and s.get("sy", 0) > 0, 30)
    check("the next launch reopens the tab on the page it showed, scrolled",
          loaded(b)(st) and st.get("tabs") == 1 and st.get("sy") == sy,
          f"url {st.get('url')} tabs {st.get('tabs')} scroll {st.get('sy')}")
    dbg.send("gui key 0xF785 alt")
    st = wait(dbg, loaded(d))
    check("...with its Forward", loaded(d)(st), f"url {st.get('url')}")
    dbg.send("gui key 0xF784 alt")
    dbg.send("gui key 0xF784 alt")
    st = wait(dbg, loaded(a))
    check("...and its Back", loaded(a)(st), f"url {st.get('url')}")


def labels(dbg):
    return [t.get("label") for t in (window(dbg) or {}).get("tabs", [])]


ENGINE_PLEDGE = "stdio rpath shm settings"


def sandboxed(dbg, host, page, at, shot):
    """The engine runs sandboxed (abi/sandbox_abi.h), and a web page's
    gets no local file. What a broken version would still pass: an
    engine that never pledged loads every page just the same, so its
    promises are read from `ps --sandbox`; a refusal that broke images
    altogether would also leave the file: one blank, so the SAME image
    over http, above it on the page, must draw (the control)."""
    url = f"http://{host}/fileref.html"
    dbg.logs("webview:", clear=True)
    go(dbg, url)
    st = wait(dbg, lambda s: loaded(url)(s) and s.get("page.pid", 0) > 0, timeout=20)
    pid = st.get("page.pid", 0)
    row = [ln for ln in dbg.send("sh ps --sandbox").splitlines()
           if ln.split()[:1] == [str(pid)]]
    check("the page's engine runs sandboxed, with only its four promises",
          bool(row) and row[0].rstrip().endswith(ENGINE_PLEDGE), f"pid {pid}: {row}")
    sx, sy = st.get("sx", 0), st.get("sy", 0)

    def box(x, y):
        return at((page[0] + x - sx - 20, page[1] + y - sy - 15, 40, 30))
    im = None
    for _ in range(20):
        im = shot("fileref")
        if count(im, box(8 + 160, 8 + 30), near((200, 40, 160), 30)) > 900:
            break
        time.sleep(0.5)
    http_img = count(im, box(8 + 160, 8 + 30), near((200, 40, 160), 30))
    file_img = count(im, box(8 + 160, 248 + 30), near((200, 40, 160), 30))
    check("an http page's image over http is drawn (control)", http_img > 900, f"{http_img} px")
    check("...and the same image as a file: URL is not", file_img < 20, f"{file_img} px")
    refused = [ln for ln in dbg.logs("webview:") if "refused file:///tests/web/bands.png" in ln]
    check("...because the app refused it to a web page", bool(refused), f"{refused[:1]}")
    log = dbg.send("sh dmesg")
    check("the engine read nothing its sandbox hid at start (locale, settings)",
          f"sandbox: pid {pid} refused path /etc/" not in log, f"pid {pid}")


# /tests/web/script.html: a script that ran adds GREEN rows; blocked, the
# RED <noscript> row shows instead. Each state is the other's control.
JS_GREEN, JS_RED = (48, 160, 64), (192, 48, 48)
SCRIPT_PAGE = "/tests/web/script.html"
OOM_PAGE = "/tests/web/oom.html"


def script_bands(im, page, at):
    box = at((page[0], page[1], page[2], min(page[3], 200)))
    return count(im, box, near(JS_GREEN, 20)), count(im, box, near(JS_RED, 20))


def js_global(dbg, value):
    out = dbg.send(f"sh config set browser.javascript {value}")
    check(f"browser.javascript set {value}", "error" not in out.lower() and "refus" not in out.lower(),
          out.strip()[-120:])


def reload_until(dbg, url, ok, timeout=15):
    # The reload must be SEEN to finish: a page whose state is the same
    # after it would otherwise pass on the state from before.
    n = state(dbg).get("loads", 0)
    dbg.send("gui key 0x12 ctrl")      # Ctrl+R
    st = wait(dbg, lambda s: s.get("loads", 0) > n and loaded(url)(s) and ok(s), timeout)
    check("...(the reload finished)", st.get("loads", 0) > n, f"loads {n} -> {st.get('loads')}")
    return st


def javascript_local(dbg, page, at, shot):
    """Stage 6: a page's scripts run by default, and the global switch
    blocks them. What a broken version would still pass: an engine that
    never ran scripts would draw the page too -- so the GREEN rows only a
    script makes must be there, and the RED <noscript> row must not; and
    off, the opposite, on the same page, so neither count is a fluke of
    where the page drew."""
    url = "file://" + SCRIPT_PAGE
    js_global(dbg, "on")
    go(dbg, SCRIPT_PAGE)
    st = wait(dbg, lambda s: loaded(url)(s) and s.get("scripts", 0) > 0)
    im = shot("js_on")
    green, red = script_bands(im, page, at)
    check("a page's scripts run: the rows only a script adds are drawn",
          st.get("js") == 1 and green > 3000, f"js {st.get('js')} green {green}")
    check("...and its <noscript> row is not (control)", red < 50, f"red {red}")

    js_global(dbg, "off")
    st = reload_until(dbg, url, lambda s: s.get("js") == 0)
    im = shot("js_off")
    green, red = script_bands(im, page, at)
    check("browser.javascript off: the page's <noscript> row is drawn", red > 1000, f"red {red}")
    check("...and its script rows are not", green < 50, f"green {green}")
    st = state(dbg)
    badge = st.get("addressbar.scripts")
    check("...and the address bar says its 1 script was blocked",
          st.get("scripts") == 1 and badge is not None, f"scripts {st.get('scripts')} badge {badge}")
    if badge:
        ink = count(im, at(badge), lambda p: max(p) < 150)
        check("...in a badge that is drawn", ink > 30, f"{ink} dark px")
        click_rect(dbg, at, badge)
        st = wait(dbg, lambda s: s.get("ask.open") == 1, 4)
        check("a click on the badge opens the Scripts blocked card", st.get("ask.open") == 1,
              f"ask.open {st.get('ask.open')}")
        dbg.send("gui key 0x1b")
        st = wait(dbg, lambda s: s.get("ask.open") == 0, 4)
        check("...and Esc closes it", st.get("ask.open") == 0, f"ask.open {st.get('ask.open')}")
    js_global(dbg, "on")


def out_of_memory(dbg):
    """A page's engine is held to its memory limit (RLIMIT_DATA, set from
    the app's --memory) and ENDS when its scripts run out, saying so --
    rather than taking the machine's last frame, or spinning in Duktape's
    garbage collector. What a broken version still passes: without the
    limit the script stops at the heap's ceiling with a JavaScript error
    and the page stays up (stopped 0); without the exit code carried
    through, the bar says "stopped" (nomem 0)."""
    go(dbg, OOM_PAGE)
    st = wait(dbg, lambda s: s.get("page.stopped") == 1, 60)
    check("a page whose script allocates without end is ended", st.get("page.stopped") == 1,
          f"stopped {st.get('page.stopped')} pid {st.get('page.pid')}")
    check("...and the bar says it ran out of memory", st.get("page.nomem") == 1,
          f"nomem {st.get('page.nomem')}")
    dmesg = dbg.send("sh dmesg") or ""
    check("...at its data limit, refused by the kernel", "reached its data limit" in dmesg,
          dmesg.strip()[-200:])
    check("...and no process lost a page to an empty machine", "no frame for user page" not in dmesg,
          "a 'no frame' line was logged")
    check("...and the app is still there", window(dbg) is not None, "no browser window")
    go(dbg, SCRIPT_PAGE)
    st = wait(dbg, lambda s: s.get("page.stopped") == 0 and loaded("file://" + SCRIPT_PAGE)(s), 15)
    check("...and the next page loads, in a new process", st.get("page.stopped") == 0 and
          loaded("file://" + SCRIPT_PAGE)(st), f"stopped {st.get('page.stopped')} url {st.get('url')}")


def javascript_sites(dbg, host, page, at, shot):
    """A site's own choice, over the global switch: "Allow on this site"
    in the Scripts blocked card, and the connection card's Default /
    Allow / Block. What a broken version would still pass: a choice that
    reached nothing still reloads the page -- so each one is read from the
    page's own pixels, and from the file it is kept in."""
    url = f"http://{host}/script.html"   # the server's root is data/tests/web
    js_global(dbg, "off")
    go(dbg, url)
    st = wait(dbg, lambda s: loaded(url)(s) and s.get("js") == 0 and s.get("scripts", 0) > 0, 20)
    badge = st.get("addressbar.scripts")
    check("an http page, JavaScript off: its scripts are blocked and the badge shows",
          st.get("js") == 0 and badge is not None, f"js {st.get('js')} badge {badge}")
    if not badge:
        js_global(dbg, "on")
        return
    click_rect(dbg, at, badge)
    st = wait(dbg, lambda s: s.get("ask.open") == 1 and "ask.primary" in s, 4)
    if check("...whose card offers Allow on this site", "ask.primary" in st, f"{st.get('ask.primary')}"):
        click_rect(dbg, at, st["ask.primary"])
        st = wait(dbg, lambda s: loaded(url)(s) and s.get("js") == 1, 15)
        green, red = script_bands(shot("js_allowed"), page, at)
        check("Allow on this site reloads the page with its scripts run",
              st.get("js") == 1 and green > 3000 and red < 50, f"js {st.get('js')} green {green} red {red}")
        kept = dbg.send("sh cat /var/lib/browser/permissions")
        check("...and keeps the choice", f"{GATEWAY}\tjavascript\tallow" in kept, kept.strip()[-120:])

    def card_choose(value):
        st = state(dbg)
        click_rect(dbg, at, st["addressbar.chip"])
        st = wait(dbg, lambda s: s.get("card_open") == 1 and value in s.get("js_slot", {}), 4)
        slot = st.get("js_slot", {}).get(value)
        if slot:
            click_rect(dbg, at, slot)
        return slot

    if card_choose(2):                  # Block
        st = wait(dbg, lambda s: loaded(url)(s) and s.get("js") == 0, 15)
        green, red = script_bands(shot("js_blocked"), page, at)
        js_global(dbg, "on")
        st = reload_until(dbg, url, lambda s: s.get("js") is not None)
        green2, red2 = script_bands(shot("js_blocked_on"), page, at)
        check("the card's Block blocks the site's scripts",
              green < 50 and red > 1000, f"green {green} red {red}")
        check("...even with JavaScript on everywhere else", st.get("js") == 0 and red2 > 1000,
              f"js {st.get('js')} red {red2}")
    else:
        check("the connection card shows a JavaScript row on a web page", False, f"{state(dbg).get('js_slot')}")
    if card_choose(0):                  # Default: the global switch, on
        st = wait(dbg, lambda s: loaded(url)(s) and s.get("js") == 1, 15)
        kept = dbg.send("sh cat /var/lib/browser/permissions")
        check("Default forgets the site's choice and follows the switch",
              st.get("js") == 1 and "javascript" not in kept, f"js {st.get('js')} kept {kept.strip()[-80:]}")
    js_global(dbg, "on")


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

    sandboxed(dbg, host, page, at, shot)
    javascript_sites(dbg, host, page, at, shot)

    # A page bigger than the network process's ring (lib/ubulk.h): it
    # arrives in pieces as the content process makes room. A broken wait
    # for room once ended the fetch half-sent, and the page never loaded.
    go(dbg, f"{host}/big.html?kb=1024")
    st = wait(dbg, loaded(f"http://{host}/big.html?kb=1024"), timeout=40)
    check("a page of 1 MiB, more than the ring holds, loads whole",
          loaded(f"http://{host}/big.html?kb=1024")(st) and "Big 1024" in labels(dbg)
          and st.get("ext_h", 0) > 100000,
          f"url={st.get('url')} loading={st.get('loading')} ext_h={st.get('ext_h')} tabs {labels(dbg)}")
    go(dbg, f"{host}/cookie/set?toy=yes")
    wait(dbg, loaded(f"http://{host}/cookie/set?toy=yes"), timeout=20)
    go(dbg, f"{host}/cookie/show")
    wait(dbg, loaded(f"http://{host}/cookie/show"), timeout=20)
    dbg.settle(1.0)
    check("a cookie the server set is sent back", "cookie toy=yes" in labels(dbg),
          f"tabs {labels(dbg)}")
    im = shot("insecure")
    st = state(dbg)
    chip = st.get("addressbar.chip")
    red = count(im, at(chip), near(DANGER, 40)) if chip else 0
    check("an http page's address field says Not secure, in red", st.get("site") == SITE_INSECURE
          and red > 30, f"site {st.get('site')} {red} red px")
    card = open_card(dbg, at)
    check("its lock's card opens, counting the site's cookie", card and card[0] == SITE_INSECURE
          and card[1] >= 1 and _st.get("card_open") == 1, f"card {card} open {_st.get('card_open')}")
    dbg.send("gui key 0x1b")
    st = wait(dbg, lambda s: s.get("card_open") == 0, 4)
    check("...and Esc closes it", st.get("card_open") == 0, f"open {st.get('card_open')}")

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

    downloads(dbg, host, at, shot)

    conns, reqs = srv.counts()
    check("the server saw fewer connections than requests (reused)",
          reqs >= 7 and conns < reqs, f"connections={conns} requests={reqs}")

    if net.get("certs"):
        dbg.send(f"gui spawn /bin/wget -O /etc/ssl/certs/saunaos-test-ca.pem http://{host}/ca.pem")
        for _ in range(40):
            if "BEGIN CERTIFICATE" in (dbg.send("sh cat /etc/ssl/certs/saunaos-test-ca.pem") or ""):
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
        check("...and the address field shows it as secure", st.get("site") == SITE_SECURE,
              f"site {st.get('site')}")
        card = open_card(dbg, at)
        check("its card names the protocol and the certificate chain", card and card[0] == SITE_SECURE
              and card[2] >= 1 and card[3].startswith("TLS"), f"card {card}")
        shot("card_https")
        dbg.send("gui key 0x1b")
        wait(dbg, lambda s: s.get("card_open") == 0, 4)
        # The same server again: a POOLED connection, no handshake -- the
        # certificate comes from the host's record.
        good2 = f"https://{GATEWAY}:{net['good'].port}/net.html"
        go(dbg, good2)
        wait(dbg, loaded(good2), timeout=40)
        card = open_card(dbg, at)
        check("...and still does after a page over a reused connection",
              card and card[0] == SITE_SECURE and card[2] >= 1, f"card {card}")
        dbg.send("gui key 0x1b")
        wait(dbg, lambda s: s.get("card_open") == 0, 4)
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
    jar = ""
    for _ in range(20):   # webnet saves as it exits, a moment after the window
        jar = dbg.send("sh cat /var/lib/browser/cookie-jar") or ""
        if "toy" in jar:
            break
        time.sleep(0.25)
    check("closing the window keeps the cookie in /var/lib/browser",
          gone and "toy" in jar and "yes" in jar, jar.strip()[-120:])
    marks = dbg.send("sh cat /var/lib/browser/bookmarks.tsv") or ""
    check("...and the bookmark in /var/lib/browser/bookmarks.tsv", "tests/web/text.html" in marks,
          marks.strip()[-160:])

    # Clear, from the card, in a new window (the jar above is checked
    # first): the cookie must then no longer be SENT.
    _st.clear()
    forget_session(dbg)
    dbg.spawn(APP + " --weak-entropy")
    wait(dbg, lambda s: "page" in s and loaded(HOME)(s), 30)
    go(dbg, f"{host}/cookie/show")
    wait(dbg, loaded(f"http://{host}/cookie/show"), timeout=20)
    card = open_card(dbg, at)
    clear = _st.get("card.clear")
    if clear:
        x, y, w, h = clear
        dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
    st = wait(dbg, lambda s: s.get("cleared") is not None and s.get("cardinfo", (0, 1))[1] == 0, 4)
    after = st.get("cardinfo")
    dbg.send("gui key 0x1b")
    # A NEW address, so the answer is the server's and not the cache's.
    go(dbg, f"{host}/cookie/show?after=clear")
    wait(dbg, loaded(f"http://{host}/cookie/show?after=clear"), timeout=20)
    dbg.settle(1.0)
    check("Clear on the card deletes the site's cookie, so it is no longer sent",
          card and card[1] >= 1 and st.get("cleared", 0) >= 1 and after and after[1] == 0
          and "cookie toy=yes" not in labels(dbg),
          f"card {card} cleared {st.get('cleared')} then {after} tabs {labels(dbg)}")
    logins(dbg, host, at, shot)
    https_fallback(dbg, host)


def https_fallback(dbg, host):
    """A host typed without a scheme is tried over https first and falls
    back to http when https cannot be reached (Chrome's HTTPS-Upgrades;
    lib/webview_proto.h, WV_NAVIGATE_UPGRADED). The test server speaks
    only http, so the TLS handshake fails at once; `--upgrade-local` has
    the browser try an address and port over https, which it otherwise
    never does. What a broken version still passes: no upgrade at all
    loads the page over http too -- so the fallback's own log line is
    asserted -- and a fallback for everything would also follow the
    https:// typed out, which must stay an error."""
    win = window(dbg)
    if win:
        dbg.send(f"gui close {win['z']}")
        dbg.settle(1.0)
    forget_session(dbg)
    _st.clear()
    dbg.spawn(APP + " --weak-entropy --upgrade-local")
    wait(dbg, lambda s: "page" in s and loaded(HOME)(s), 30)
    go(dbg, f"{host}/net.html")
    url = f"http://{host}/net.html"
    st = wait(dbg, lambda s: loaded(url)(s) and s.get("fallback"), 30)
    check("a typed host whose https does not answer falls back to http", loaded(url)(st) and
          st.get("fallback") == host, f"url {st.get('url')} fallback {st.get('fallback')}")
    st = wait(dbg, lambda s: s.get("site") == SITE_INSECURE, 5)
    check("...and the address field says Not secure", st.get("site") == SITE_INSECURE, f"site {st.get('site')}")

    go(dbg, f"{host}/table.html")
    url = f"http://{host}/table.html"
    st = wait(dbg, loaded(url), 20)
    dbg.settle(1.0)
    st = state(dbg)
    check("...and the host is tried over http first from then on", loaded(url)(st) and
          st.get("fallbacks") == 1, f"url {st.get('url')} fallbacks {st.get('fallbacks')}")

    go(dbg, f"https://{host}/text.html")
    st = wait(dbg, lambda s: s.get("loading") == 0 and "text.html" in s.get("url", ""), 20)
    dbg.settle(2.0)
    st = state(dbg)
    check("an https:// typed out never falls back", st.get("fallbacks") == 1 and
          not st.get("url", "").startswith("http:"), f"url {st.get('url')} fallbacks {st.get('fallbacks')}")


def logins(dbg, host, at, shot):
    """HTTP Basic, answered in the app and kept by /bin/webnet
    (lib/webnet_proto.h, WN_AUTH_ASK): the 401 is held while the sign-in
    is up, a wrong password asks again, the right one loads the page,
    another tab -- another content process -- is let in without asking,
    the lock card's Clear forgets it, and Cancel shows the 401's own page.
    What a broken version still passes: NetSurf's in-page form would log
    no sign-in at all; a login kept per content process would ask again
    in the second tab."""
    _st.pop("login", None)
    _st.pop("logins", None)
    go(dbg, f"{host}/auth/a.html")
    st = wait(dbg, lambda s: s.get("login"), 20)
    check("a 401 opens the sign-in, saying the page is not secure", st.get("login") and
          st["login"][1] == 0 and st["login"][2] == 0, f"login {st.get('login')}")
    # Drawn, not just logged: the form's white fields, and the accent of
    # Sign in, the default button, in the row just below the form.
    form = _st.get("signin")
    box = (form[0], form[1], form[2], form[3] + 70) if form else None
    im = shot("signin")
    white = count(im, at(box), lambda p: min(p) > 245) if box else 0
    blue = count(im, at(box), near(ACCENT, 16)) if box else 0
    check("...and it is drawn: white fields, an accent Sign in", box and white > 2000 and blue > 300,
          f"box {box} white {white} accent {blue}")
    type_text(dbg, "toy")
    dbg.send("gui key 0x09")
    type_text(dbg, "wrong")
    dbg.send("gui key 0x0d")
    st = wait(dbg, lambda s: s.get("logins", 0) >= 2, 20)
    check("...a wrong password asks again", st.get("login", (0, 0))[1] == 1, f"login {st.get('login')}")
    type_text(dbg, "secret")      # the user name is kept, the password focused
    dbg.send("gui key 0x0d")
    url = f"http://{host}/auth/a.html"
    st = wait(dbg, loaded(url), 20)
    dbg.settle(1.0)
    check("...and the right one loads the page", loaded(url)(st) and "signed in /auth/a.html" in labels(dbg),
          f"url {st.get('url')} tabs {labels(dbg)}")

    n = _st.get("logins", 0)
    dbg.send("gui key 0x14 ctrl")      # Ctrl+T: another content process
    wait(dbg, lambda s: s.get("tabs") == 2 and loaded(HOME)(s))
    go(dbg, f"{host}/auth/b.html")
    url = f"http://{host}/auth/b.html"
    st = wait(dbg, loaded(url), 20)
    dbg.settle(1.0)
    check("another tab is let in without asking: webnet kept the login",
          loaded(url)(st) and _st.get("logins", 0) == n and "signed in /auth/b.html" in labels(dbg),
          f"url {st.get('url')} sign-ins {_st.get('logins')} (was {n}) tabs {labels(dbg)}")

    card = open_card(dbg, at)
    kept = _st.get("card_logins")
    clear = _st.get("card.clear")
    check("the lock card lists the sign-in among the site's data, with Clear", kept == 1 and clear,
          f"logins {kept} clear {clear}")
    if clear:
        x, y, w, h = clear
        dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
        dbg.settle(1.0)
    dbg.send("gui key 0x1b")
    go(dbg, f"{host}/auth/c.html")
    st = wait(dbg, lambda s: s.get("logins", 0) > n, 20)
    check("the lock card's Clear forgets the login: the next page asks", card and st.get("logins", 0) > n,
          f"card {card} sign-ins {st.get('logins')}")
    dbg.send("gui key 0x1b")           # Esc: Cancel
    url = f"http://{host}/auth/c.html"
    st = wait(dbg, lambda s: loaded(url)(s) and s.get("login_done") == "cancelled", 20)
    dbg.settle(1.0)
    check("...and Cancel shows the server's own 401 page", loaded(url)(st) and "auth refused" in labels(dbg),
          f"url {st.get('url')} done {st.get('login_done')} tabs {labels(dbg)}")
    if state(dbg).get("tabs") == 2:    # never the last tab: that closes the window
        dbg.send("gui key 0x17 ctrl")  # Ctrl+W
        wait(dbg, lambda s: s.get("tabs") == 1, 6)


def dl_event(ev, name):
    return lambda s: (ev, name) in [(e, n) for e, n, _ in s.get("dl", [])]


def guest_crc(dbg, path):
    """`sum`'s CRC-32 and length of a guest file, or None."""
    out = dbg.send(f"sh sum {path}") or ""
    m = re.search(r"(\d+) (\d+) " + re.escape(path), out)
    return (int(m.group(1)), int(m.group(2))) if m else None


def downloads(dbg, host, at, shot):
    page_before = state(dbg).get("url")
    size = 300000
    want = (zlib.crc32(download_bytes(size)), size)
    go(dbg, f"{host}/download/data.bin?size={size}")
    st = wait(dbg, lambda s: dl_event("done", "data.bin")(s) and s.get("dlp_open") == 1, 30)
    check("an attachment downloads, and the Downloads flyout opens for it",
          dl_event("done", "data.bin")(st) and st.get("dlp_open") == 1,
          f"events {st.get('dl')} open {st.get('dlp_open')}")
    got = guest_crc(dbg, "/home/Downloads/data.bin")
    check("...into /home/Downloads, byte for byte", got == want, f"sum {got}, want {want}")
    part = dbg.send("sh ls /home/Downloads") or ""
    check("...leaving no .part behind, and the page it came from shown", ".part" not in part
          and state(dbg).get("url") == page_before, f"ls: {part.strip()[-120:]}")
    go(dbg, f"{host}/download/data.bin?size=1000")
    st = wait(dbg, dl_event("done", "data (1).bin"), 30)
    check("a second file of the same name is saved beside it, as 'data (1).bin'",
          dl_event("done", "data (1).bin")(st), f"events {st.get('dl')}")

    go(dbg, f"{host}/download/broken.bin?size={size}&drop=5000")
    st = wait(dbg, dl_event("failed", "broken.bin"), 30)
    ls = dbg.send("sh ls /home/Downloads") or ""
    check("a download whose connection breaks fails, and leaves nothing",
          dl_event("failed", "broken.bin")(st) and "broken" not in ls,
          f"events {st.get('dl')} ls: {ls.strip()[-120:]}")

    _st.pop("dl_action", None)
    go(dbg, f"{host}/download/slow.bin?size=2000000&slow=1")
    # Row 0's action must be the new row's SQUARE Cancel: a frame drawn
    # before the row arrived reports broken.bin's wider Retry there.
    cancel = lambda s: s.get("dl_action", {}).get(0, (0, 0, 1, 0))
    st = wait(dbg, lambda s: dl_event("start", "slow.bin")(s) and cancel(s)[2] == cancel(s)[3], 20)
    act = st.get("dl_action", {}).get(0)
    if act:
        x, y, w, h = act
        dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
    st = wait(dbg, dl_event("cancelled", "slow.bin"), 10)
    ls = dbg.send("sh ls /home/Downloads") or ""
    check("Cancel on a running download's row stops it and leaves nothing",
          dl_event("cancelled", "slow.bin")(st) and "slow" not in ls,
          f"events {st.get('dl')} ls: {ls.strip()[-120:]}")

    shot("downloads")
    clear = state(dbg).get("dlpanel.clear")
    if clear:
        x, y, w, h = clear
        dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
    st = wait(dbg, lambda s: s.get("dl_count") == 0, 6)
    check("Clear finished empties the list", st.get("dl_count") == 0, f"count {st.get('dl_count')}")
    dbg.send("gui key 0x1b")


def open_card(dbg, at):
    """Clicks the address field's chip; the card's report, or None."""
    _st.pop("cardinfo", None)
    chip = state(dbg).get("addressbar.chip")
    if not chip:
        return None
    x, y, w, h = chip
    dbg.send("gui click %d %d" % (at((x + w // 2, y + h // 2, 0, 0))[:2]))
    return wait(dbg, lambda s: "cardinfo" in s and s.get("card_open") == 1, 6).get("cardinfo")


def finish():
    return _res.finish("browser_test")


if __name__ == "__main__":
    sys.exit(main())
