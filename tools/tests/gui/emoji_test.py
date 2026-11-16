#!/usr/bin/env python3
"""Emoji draw in COLOUR from the fallback list's emoji face, and U+FE0F decides.

WHY THIS EXISTS. A character the interface face lacks is drawn by the first
face in /etc/fonts/fallback.conf that has it, and an emoji -- Unicode's
Emoji_Presentation, or a character followed by U+FE0F -- goes to the emoji
face first, drawn from its COLR/CPAL colour layers (ttf.h). Every step can
fail quietly: a missing face draws text or boxes, a lost selector draws a
black heart, a colour path that broke draws the layers in the text colour.
Only the pixels can tell.

THE CHECKS, each one a broken version would fail. Notepad opens three files
and the editor's own rectangle (between its `tabs` and `status` widgets) is
read:
  - four U+1F600 draw Twemoji's yellow face (a colour no text is drawn in);
  - four U+2764 U+FE0F draw its red heart;
  - four U+2764 with no selector draw NO red: a text heart, in the ink.

`--positive-control` first rewrites the guest's /etc/fonts/fallback.conf
WITHOUT the emoji face, so the first two checks must go red -- the same
images, the same reads, only the face gone.

  python3 tools/tests/gui/emoji_test.py [--instance N] [--positive-control]
"""

RUNNER = {
    "covers": "emoji in colour from the fallback list's emoji face; U+FE0F red heart, bare U+2764 not",
}

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                     # noqa: E402
from PIL import Image                           # noqa: E402

FILES = {
    "/tmp/emoji_face.txt":  "\U0001F600" * 4,
    "/tmp/emoji_heart.txt": "❤️" * 4,
    "/tmp/emoji_text.txt":  "❤" * 4,
}
NO_EMOJI_LIST = ["noto-sans", "noto-sans-symbols2", "noto-sans-symbols", "noto-sans-math",
                 "dejavu-sans-mono"]


def yellow(p):
    r, g, b = p[:3]
    return r > 220 and 150 < g < 230 and b < 110


def red(p):
    r, g, b = p[:3]
    return r > 170 and g < 90 and b < 110


def editor_pixels(dbg, qmp, title, tmp):
    """The editor's pixels in the window titled `title`, or None."""
    if not dbg.window_settled(title):
        return None
    wd = dbg.widgets(title)
    if "tabs" not in wd or "status" not in wd:
        return None
    x0 = wd["tabs"]["screen"]["x"]
    y0 = wd["tabs"]["screen"]["y"] + wd["tabs"]["h"]
    y1 = wd["status"]["screen"]["y"]
    x1 = x0 + wd["tabs"]["w"]
    png = os.path.join(tmp, "shot.png")
    qmp.screenshot(png)
    raw = Image.open(png).convert("RGB").crop((x0, y0, x1, y1)).tobytes()
    return [raw[i:i + 3] for i in range(0, len(raw), 3)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="remove the emoji face from the guest's list; the colour checks must go RED")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "emoji_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("emoji: colour glyphs from the fallback list's emoji face")
    if args.positive_control:
        res.check("control: the list without the emoji face is written",
                  dbg.write_lines("/etc/fonts/fallback.conf", NO_EMOJI_LIST))
    for path, text in FILES.items():
        res.check(f"{path} is written", dbg.write_lines(path, [text]))
    with tempfile.TemporaryDirectory() as tmp:
        seen = {}
        for path in FILES:
            dbg.send(f"gui spawn /bin/wm/apps/notepad {path}")
            seen[path] = editor_pixels(dbg, qmp, path, tmp)
            res.check(f"Notepad shows {path}", seen[path] is not None)
            dbg.close_window(path)
        face = seen["/tmp/emoji_face.txt"] or []
        heart = seen["/tmp/emoji_heart.txt"] or []
        text = seen["/tmp/emoji_text.txt"] or []
        n_yellow = sum(1 for p in face if yellow(p))
        n_red = sum(1 for p in heart if red(p))
        n_red_text = sum(1 for p in text if red(p))
        res.check("U+1F600 draws in colour (Twemoji's yellow face)", n_yellow > 40,
                  f"{n_yellow} yellow pixels")
        res.check("U+2764 U+FE0F draws the red heart", n_red > 40, f"{n_red} red pixels")
        res.check("a bare U+2764 draws as text: no red", n_red_text == 0,
                  f"{n_red_text} red pixels")
    return res.finish("emoji_test")


if __name__ == "__main__":
    sys.exit(main())
