#!/usr/bin/env python3
"""Chinese, Japanese and Korean text draws from the fallback list's CJK face, its member by index.

WHY THIS EXISTS. The interface face has no Han, kana or Hangul, so those
characters come from Noto Sans CJK -- a COLLECTION (`.ttc`) of CFF fonts,
named in /etc/fonts/fallback.conf as `noto-sans-cjk-regular:2`, mapped
rather than read into each process (the system image cache shares its
pages). Every step can fail quietly: an unread collection or CFF outline
draws the outlined box ugfx draws for a character nothing has, a lost
`:N` draws another region's shapes, and a face read whole costs every
process the whole file. Only pixels and the process table can tell.

THE CHECKS, each one a broken version would fail. Notepad opens files and
the editor's rectangle (between its `tabs` and `status` widgets) is read:
  - four 龍 hold over twice the ink of four 一, and four 힣 over 1.6 times
    it (Hangul, from the same face) -- as boxes all three are the same
    picture;
  - four 骨 under `:2` (Simplified Chinese) and under `:0` (Japanese, the
    list rewritten, a new Notepad) differ: the member index reaches the
    glyphs;
  - the Notepad drawing them holds under half the face's size (the
    file's own, read from the tree).

`--positive-control` rewrites the list WITHOUT the CJK face first: the ink
checks must go RED. Either way the machine's own list is copied aside first
and copied back, byte for byte, however the run ends.

  python3 tools/tests/gui/cjk_text_test.py [--instance N] [--positive-control]
"""

RUNNER = {
    "covers": "CJK text from Noto Sans CJK (a .ttc member, CFF outlines); the member index honoured; "
              "the face mapped, not copied",
}

import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402
from PIL import Image                           # noqa: E402

CONF = "/etc/fonts/fallback.conf"
SAVED = "/tmp/cjk_text_test.fallback.conf"
LIST = ["noto-sans", "noto-sans-symbols2", "noto-sans-symbols", "noto-sans-math",
        "noto-sans-cjk-regular:2", "emoji twemoji", "dejavu-sans-mono"]
FILES = {
    "/tmp/cjk_one.txt": "一" * 4,
    "/tmp/cjk_dragon.txt": "龍" * 4,
    "/tmp/cjk_hangul.txt": "힣" * 4,
    "/tmp/cjk_bone.txt": "骨" * 4,
}
FACE_FILE = os.path.join(toolpath.REPO, "data", "fonts", "fallback", "noto-sans-cjk-regular.ttc")


def editor_ink(dbg, qmp, title, tmp):
    """(dark pixel count, the editor's pixels) in the window titled `title`, or (None, None)."""
    if not dbg.window_settled(title):
        return None, None
    wd = dbg.widgets(title)
    if "tabs" not in wd or "status" not in wd:
        return None, None
    x0 = wd["tabs"]["screen"]["x"]
    y0 = wd["tabs"]["screen"]["y"] + wd["tabs"]["h"]
    y1 = wd["status"]["screen"]["y"]
    x1 = x0 + wd["tabs"]["w"]
    png = os.path.join(tmp, "shot.png")
    qmp.screenshot(png)
    raw = Image.open(png).convert("L").crop((x0, y0, x1, y1)).tobytes()
    return sum(1 for v in raw if v < 128), raw


def notepad_kib(dbg):
    """The MEM(K) column of the newest Notepad in `ps`, or None."""
    rows = [ln.split() for ln in dbg.send("sh ps").splitlines() if ln.rstrip().endswith("notepad")]
    kib = [int(r[5]) for r in rows if len(r) >= 7 and re.fullmatch(r"\d+", r[5])]
    return kib[-1] if kib else None


def show(dbg, qmp, path, tmp):
    dbg.send(f"gui spawn /bin/wm/apps/notepad {path}")
    ink, raw = editor_ink(dbg, qmp, path, tmp)
    kib = notepad_kib(dbg)
    dbg.close_window(path)
    return ink, raw, kib


def run(dbg, qmp, res, control):
    conf = [ln for ln in LIST if not ln.startswith("noto-sans-cjk")] if control else LIST
    res.check("the fallback list is written" + (" WITHOUT the CJK face (control)" if control else ""),
              dbg.write_lines(CONF, conf))
    for path, text in FILES.items():
        res.check(f"{path} is written", dbg.write_lines(path, [text]))
    with tempfile.TemporaryDirectory() as tmp:
        seen = {path: show(dbg, qmp, path, tmp) for path in FILES}
        for path in FILES:
            res.check(f"Notepad shows {path}", seen[path][0] is not None)
        one = seen["/tmp/cjk_one.txt"][0] or 0
        dragon = seen["/tmp/cjk_dragon.txt"][0] or 0
        hangul = seen["/tmp/cjk_hangul.txt"][0] or 0
        # Four outlined boxes hold about the ink of four 一 (measured: the
        # control's boxes against the face's 一), so the bars sit above that.
        res.check("four 龍 hold over twice the ink of four 一 (boxes would match)", dragon > 2 * max(one, 1),
                  f"龍 {dragon} dark pixels, 一 {one}")
        res.check("four 힣 hold over 1.6 times it (Hangul from the same face)", hangul > 1.6 * max(one, 1),
                  f"힣 {hangul}, 一 {one}")
        kib = seen["/tmp/cjk_dragon.txt"][2]
        face_kib = os.path.getsize(FACE_FILE) // 1024
        res.check(f"the Notepad drawing them holds under half the face's {face_kib} KiB",
                  kib is not None and kib < face_kib / 2, f"{kib} KiB")
        if control:
            return
        sc = seen["/tmp/cjk_bone.txt"][1]
        res.check("the list naming member :0 instead is written",
                  dbg.write_lines(CONF, [ln.replace("cjk-regular:2", "cjk-regular:0") for ln in LIST]))
        _, jp, _ = show(dbg, qmp, "/tmp/cjk_bone.txt", tmp)
        differ = sc is not None and jp is not None and len(sc) == len(jp) and \
            sum(1 for a, b in zip(sc, jp) if abs(a - b) > 64)
        res.check("骨 under :2 (SC) and :0 (JP) draws differently: the member index reaches the glyphs",
                  bool(differ), f"{differ} pixels differ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="remove the CJK face from the guest's list; the ink checks must go RED")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "cjk_text_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("cjk: Han and Hangul from the fallback list's CJK collection")
    # The machine's own list is set aside first and put back however the
    # run ends: a test that leaves a setting changed changes every tool
    # after it (CLAUDE.md).
    # `sh` returns before its command is done: each copy is confirmed by
    # reading it back.
    original = dbg.send(f"sh cat {CONF}") or ""
    dbg.send(f"sh cp {CONF} {SAVED}")
    saved = "noto" in original and poll(lambda: (dbg.send(f"sh cat {SAVED}") or "") == original, 10)
    res.check("the machine's own list is set aside", bool(saved))
    if not saved:
        return res.finish("cjk_text_test")
    try:
        run(dbg, qmp, res, args.positive_control)
    finally:
        dbg.send(f"sh cp {SAVED} {CONF}")
        res.check("the machine's own list is put back, as it was",
                  bool(poll(lambda: (dbg.send(f"sh cat {CONF}") or "") == original, 10)))
    return res.finish("cjk_text_test")


if __name__ == "__main__":
    sys.exit(main())
