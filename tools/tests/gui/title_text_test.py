#!/usr/bin/env python3
"""A window title or tab label longer than one channel message
(abi/win_proto.h's WIN_TEXT_PIECE): it travels in pieces, the compositor
keeps all of it, and a title too long for its bar ends in `..`.

1. **A 300-byte title arrives whole** -- a two-byte character straddles
   the 31-byte pieces, so a join that cut or dropped one shows here.
2. **Two 300-byte tab labels arrive whole**, each its own text.
3. **A short title is still one message** and arrives as it was.
4. **A title past WIN_TEXT_MAX is cut on a character boundary** at the
   wire's bound, never refused or garbled.
5. **The long title's text runs up to the caption buttons, the short
   one's does not** -- read from pixels, not the window list.

The text is /tests/titleclient's ("Äbc " repeated, cut to N bytes on a
character boundary), rebuilt here byte for byte.
"""

RUNNER = {"name": "titletext",
          "covers": "long titles and tab labels in pieces, the wire bound, the title bar's ink"}

import argparse
import re
import sys

import os  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
import harness  # noqa: E402
from harness import Results  # noqa: E402

CLIENT = "/tests/titleclient"
WIN_TEXT_MAX = 1024
UNIT = "Äbc ".encode()   # 5 bytes, as titleclient.c's


def make(prefix, n):
    out = bytearray(prefix.encode())
    base = len(out)
    while len(out) < n:
        k = (len(out) - base) % len(UNIT)
        if k == 0 and len(out) + 2 > n:
            break
        out += UNIT[k:k + 2] if k == 0 else UNIT[k:k + 1]
    return out.decode()


def cut(text, limit):
    """What the client sends past the wire's bound: at most `limit`
    bytes, ending on a character boundary."""
    return text.encode()[:limit].decode("utf-8", errors="ignore")


def open_client(dbg, args, want, timeout=15.0, ready=None):
    """Spawns titleclient and waits for a window titled `want` -- and, with
    `ready`, until ready(window) is true too: its tabs come after its title,
    so a wait on the title alone reads a window with no tabs yet. Returns
    (window, pid); the window is the last one seen if it never matched."""
    out = dbg.send(f"gui spawn {CLIENT} {args}") or ""
    m = re.search(r"as pid (\d+)", out)
    pid = int(m.group(1)) if m else None
    seen = []

    def match():
        for w in dbg.windows():
            if w["title"].startswith(want[:8]):
                seen[:] = [w]
                if w["title"] == want and (ready is None or ready(w)):
                    return w
        return None
    win = harness.poll(match, timeout)
    if win:
        dbg.settle()   # its first frame composited, for the pixels
    return win or (seen[0] if seen else None), pid


def close_client(dbg, pid, want):
    """Ends the client and waits for its window to go."""
    if not pid:
        return
    dbg.send(f"sh kill {pid}")
    harness.poll(lambda: not any(w["title"] == want for w in dbg.windows()), 10)


def ink_right_of_middle(qmp, win, path):
    """Pixels in the title bar's right half, short of the caption
    buttons, that differ from the bar -- text reaching that far."""
    from PIL import Image
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    x, y, w = win["x"], win["y"], win["w"]
    bar = win["content"]["y"] - y
    x0, x1 = x + w // 2, x + w - 4 * bar
    y0, y1 = y + 3, y + bar - 3
    bg = im.getpixel((x0, y0))
    ink = 0
    for yy in range(y0, y1):
        for xx in range(x0, x1):
            p = im.getpixel((xx, yy))
            if sum(abs(p[i] - bg[i]) for i in range(3)) > 120:
                ink += 1
    return ink


def run(dbg, qmp, res, scratch):
    # 1. A long title, in pieces.
    long_title = make("", 300)
    win, pid = open_client(dbg, "300", long_title)
    got = win["title"] if win else None
    res.check("a 300-byte title arrives whole, across its pieces",
              got == long_title,
              f"got {len((got or '').encode())} bytes: {got!r:.80}")
    long_ink = ink_right_of_middle(qmp, win, os.path.join(scratch, "long.png")) if win else 0
    close_client(dbg, pid, long_title)

    # 2. Long tab labels.
    want = [make("A:", 300), make("B:", 300)]
    win, pid = open_client(dbg, "300 tabs", long_title,
                           ready=lambda w: [t.get("label") for t in w.get("tabs", [])] == want)
    labels = [t.get("label") for t in (win or {}).get("tabs", [])]
    res.check("two 300-byte tab labels arrive whole, each its own",
              labels == want,
              f"got {[len((l or '').encode()) for l in labels]} bytes")
    close_client(dbg, pid, long_title)

    # 3. A short title: one message, as before pieces.
    short = make("", 20)
    win, pid = open_client(dbg, "20", short)
    res.check("a short title is one message and arrives as it was",
              win is not None and win["title"] == short,
              f"got {(win or {}).get('title')!r}")
    short_ink = ink_right_of_middle(qmp, win, os.path.join(scratch, "short.png")) if win else -1
    close_client(dbg, pid, short)

    # 4. Past the wire's bound.
    huge = make("", 2000)
    want_cut = cut(huge, WIN_TEXT_MAX - 1)
    win, pid = open_client(dbg, "2000", want_cut)
    got = (win or {}).get("title", "")
    res.check(f"a 2000-byte title is cut on a character boundary at the bound "
              f"({len(want_cut.encode())} bytes)",
              got == want_cut, f"got {len(got.encode())} bytes")
    close_client(dbg, pid, want_cut)

    # 5. Pixels: the bar, not the list.
    res.check("the long title's text runs up to the caption buttons",
              long_ink > 20, f"{long_ink} ink pixels right of the middle")
    res.check("and the short title's does not (the control)",
              short_ink == 0, f"{short_ink} ink pixels right of the middle")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "title_text_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    scratch = harness.scratch_dir("title_text_test")
    try:
        run(dbg, qmp, res, scratch)
    finally:
        dbg.close()
    print(f"\ntitle_text_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
