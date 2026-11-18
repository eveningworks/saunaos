#!/usr/bin/env python3
"""The emoji picker (Super+period) types an emoji into the focused window, and Character Map lists them in colour.

WHY THIS EXISTS. The picker is a compositor overlay (userland/apps/system/toywm/emoji_picker.c) that opens at the
caret a client reports (WIN_REQ_CARET_RECT), searches base/uemojidata.h's list, and types the pick into the focused
window as 32-bit key code points -- a non-BMP character and its U+FE0F through the same key path a keyboard uses.
Each step can fail quietly: a caret never reported puts the panel in the middle, a search that does not filter
still shows emoji, a key path that cuts a code point to 16 bits types the wrong character, and recents that are
not written still leave a working picker. Only the file, the log and the pixels can tell.

THE CHECKS, each one a broken version would fail. Over Notepad holding /tmp/picker.txt:
  1. Super+period opens the panel BELOW (or, at the screen's foot, above) Notepad's reported caret, and its first
     cell draws Twemoji's yellow face;
  2. typing "heart" narrows the grid to fewer emoji than the group listed, each named with "heart";
  3. Enter types the focused one: the panel closes, the compositor logs the insert, and the file Notepad saves holds
     its UTF-8 -- a code point past U+FFFF -- and the editor draws it in colour;
  4. opened again, the Recently used row starts with it, and a click on that cell types it a second time;
  5. Esc closes the panel;
  5b. "waving hand" focuses a hand that takes tones; a right-click on it opens the tone strip (the panel stays), Esc
     shuts only the strip, and Right x3 then Enter types the MEDIUM tone -- U+1F44B U+1F3FD, two keys the field
     joins into one emoji -- which the saved file holds;
  6. over the Terminal, which reports no caret, the panel centres in the window's lower half, and "red heart" typed
     into the shell draws RED in the Terminal's cells -- U+FE0F joined into the heart's own cell decides it;
  7. Character Map's Emoji page draws its grid in colour, and ":grinning" then Tab in its search field completes
     to the emoji itself, which the search then finds alone.

  python3 tools/tests/gui/emoji_picker_test.py [--instance N]
"""

RUNNER = {
    "covers": "the emoji picker (Super+period): caret placement, search, insert into Notepad and the Terminal, "
              "recents; the Terminal's U+FE0F red heart; Character Map's Emoji page in colour; :name: completion",
}

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402
from PIL import Image                           # noqa: E402

PATH = "/tmp/picker.txt"
CHARMAP = "Character Map"
K_ENTER, K_ESC, K_TAB, K_SAVE, K_END = "0x0a", "0x1b", "0x09", "0x13", "0xf787"
K_RIGHT = "0xf785"


def red(p):
    r, g, b = p[:3]
    return r > 170 and g < 90 and b < 110


def yellow(p):
    r, g, b = p[:3]
    return r > 220 and 150 < g < 230 and b < 110


def count_yellow(qmp, tmp, rect, test=yellow):
    x, y, w, h = rect
    png = os.path.join(tmp, "shot.png")
    qmp.screenshot(png)
    raw = Image.open(png).convert("RGB").crop((x, y, x + w, y + h)).tobytes()
    return sum(1 for i in range(0, len(raw), 3) if test(raw[i:i + 3]))


def picker(dbg):
    return dbg.json("gui emoji --json")


def read_file(dbg, path):
    out = dbg.send(f"readfile {path}")
    m = re.search(r"<<<FILE (-?\d+)\n(.*)\n>>>END", out, re.S)
    return m.group(2) if m and int(m.group(1)) >= 0 else None


def typed(dbg, text):
    for ch in text:
        dbg.key("0x20" if ch == " " else ch, settle=False)   # `gui key  ` would send nothing
    dbg.settle()


def chars(e):
    return chr(int(e["cp"], 16)) + ("️" if e["vs16"] else "")


def notepad_part(dbg, qmp, res, tmp):
    res.check(f"{PATH} is written", dbg.write_lines(PATH, ["x"]))
    dbg.send(f"gui spawn /bin/wm/apps/notepad {PATH}")
    win = dbg.window_settled(PATH)
    if not res.check("Notepad opens", win is not None):
        return
    wd = poll(lambda: dbg.widgets(PATH).get("status") and dbg.widgets(PATH), 10, 0.3) or {}
    dbg.key(K_END, mods="ctrl")   # the caret after the text
    dbg.logs("emoji:", clear=True)

    # 1. Open, at the caret.
    dbg.key(".", mods="super")
    j = picker(dbg)
    if not res.check("Super+period opens the picker", j.get("open"), str(j)):
        return
    caret = j["caret"]
    c = win["content"]
    res.check("Notepad reported its caret, inside its window",
              caret["known"] and c["x"] <= caret["x"] < c["x"] + c["w"] and c["y"] <= caret["y"] < c["y"] + c["h"],
              f"caret {caret}, content {c}")
    below = j["y"] >= caret["y"] + caret["h"]
    above = j["y"] + j["h"] <= caret["y"]
    res.check("the panel opens at the caret: under it (or above it at the screen's foot)",
              (below and j["y"] - caret["y"] - caret["h"] < 40) or (above and caret["y"] - j["y"] - j["h"] < 40),
              f"panel y={j['y']} h={j['h']}, caret y={caret['y']} h={caret['h']}")
    cell = j.get("cell")
    n_y = count_yellow(qmp, tmp, (cell["x"], cell["y"], cell["w"], cell["h"])) if cell else 0
    res.check("its first cell draws Twemoji's yellow face", n_y > 40, f"{n_y} yellow pixels in {cell}")
    group = j["listed"]

    # 2. Search.
    typed(dbg, "heart")
    j = picker(dbg)
    res.check("typing goes to the search field", j["query"] == "heart", f"query {j['query']!r}")
    res.check("a search narrows the grid", 0 < j["listed"] < group, f"{group} -> {j['listed']} ({j['caption']})")
    res.check("the focused match is named with the query",
              bool(j["focused"]) and "heart" in j["focused"]["name"], str(j["focused"]))
    want = j["focused"]

    # 3. Insert.
    dbg.key(K_ENTER)
    j = picker(dbg)
    res.check("Enter closes the picker", not j["open"])
    logged = poll(lambda: [ln for ln in dbg.logs("emoji: inserted", clear=False) if f"U+{want['cp']}" in ln], 10, 0.3)
    res.check("the compositor typed the focused emoji", bool(logged), str(logged))
    dbg.key(K_SAVE)
    text = poll(lambda: (lambda t: t if t and chars(want) in t else None)(read_file(dbg, PATH)), 10, 0.5)
    res.check(f"the saved file holds U+{want['cp']} (past U+FFFF: {int(want['cp'], 16) > 0xFFFF}) as UTF-8",
              text is not None and int(want["cp"], 16) > 0xFFFF,
              repr(read_file(dbg, PATH)))
    if "tabs" in wd and "status" in wd:
        x0, y0 = wd["tabs"]["screen"]["x"], wd["tabs"]["screen"]["y"] + wd["tabs"]["h"]
        rect = (x0, y0, wd["tabs"]["w"], wd["status"]["screen"]["y"] - y0)
        n_y = count_yellow(qmp, tmp, rect)
        res.check("Notepad draws it in colour", n_y > 40, f"{n_y} yellow pixels")

    # 4. Recents.
    dbg.key(".", mods="super")
    j = picker(dbg)
    res.check("the Recently used row starts with it", j["open"] and j["recent_row"] and j["recent"][:1] == [want["cp"]],
              f"recent {j.get('recent')}")
    rc = j["recent_cell"]
    dbg.logs("emoji: inserted", clear=True)
    dbg.click(rc["x"] + rc["w"] // 2, rc["y"] + rc["h"] // 2)
    j = picker(dbg)
    logged = poll(lambda: [ln for ln in dbg.logs("emoji: inserted", clear=False) if f"U+{want['cp']}" in ln], 10, 0.3)
    res.check("a click on a recent cell types it and closes the picker", bool(logged) and not j["open"], str(logged))
    dbg.key(K_SAVE)
    text = poll(lambda: (lambda t: t if t and t.count(chars(want)) == 2 else None)(read_file(dbg, PATH)), 10, 0.5)
    res.check("the file now holds it twice", text is not None, repr(read_file(dbg, PATH)))

    # 5. Esc.
    dbg.key(".", mods="super")
    opened = picker(dbg)["open"]
    dbg.key(K_ESC)
    res.check("Esc closes the picker", opened and not picker(dbg)["open"])

    # 6. Skin tones: a right-click on a hand opens its six forms; Esc shuts
    # only them; Right three times and Enter types the MEDIUM tone, the
    # sequence U+1F44B U+1F3FD, as two keys the field joins into one.
    dbg.key(".", mods="super")
    typed(dbg, "waving hand")
    j = picker(dbg)
    cell = j.get("cell")
    res.check("'waving hand' focuses U+1F44B, which takes tones",
              bool(j["focused"]) and j["focused"]["cp"] == "1F44B" and j["focused"].get("tones") == 1, str(j["focused"]))
    if cell:
        dbg.rclick(cell["x"] + cell["w"] // 2, cell["y"] + cell["h"] // 2)
    j = picker(dbg)
    res.check("a right-click on it opens the tone strip, the picker still up",
              j["open"] and j.get("strip", {}).get("open"), str(j.get("strip")))
    dbg.key(K_ESC)
    j = picker(dbg)
    res.check("Esc shuts the strip, not the picker", j["open"] and not j.get("strip", {}).get("open"), str(j.get("strip")))
    if cell:
        dbg.rclick(cell["x"] + cell["w"] // 2, cell["y"] + cell["h"] // 2)
    for _ in range(3):
        dbg.key(K_RIGHT)
    dbg.logs("emoji: inserted", clear=True)
    dbg.key(K_ENTER)
    logged = poll(lambda: [ln for ln in dbg.logs("emoji: inserted", clear=False) if "U+1F44B U+1F3FD" in ln], 10, 0.3)
    res.check("Right x3 then Enter types the medium tone", bool(logged) and not picker(dbg)["open"], str(logged))
    dbg.key(K_SAVE)
    toned = "\U0001F44B\U0001F3FD"
    text = poll(lambda: (lambda t: t if t and toned in t else None)(read_file(dbg, PATH)), 10, 0.5)
    res.check("the saved file holds U+1F44B U+1F3FD", text is not None, repr(read_file(dbg, PATH)))
    dbg.close_window(PATH)


def terminal_part(dbg, qmp, res, tmp):
    dbg.send("gui spawn /bin/wm/apps/uterm")
    win = poll(lambda: dbg.window_settled("Terminal"), 15, 0.3)
    if not res.check("the Terminal opens", win is not None):
        return
    c = win["content"]
    dbg.settle()
    before = count_yellow(qmp, tmp, (c["x"], c["y"], c["w"], c["h"]), red)
    dbg.key(".", mods="super")
    j = picker(dbg)
    mid = j["x"] + j["w"] // 2
    res.check("with no caret reported, the panel centres in the window's lower half",
              j["open"] and not j["caret"]["known"] and abs(mid - (c["x"] + c["w"] // 2)) < 8 and
              j["y"] + j["h"] // 2 >= c["y"] + c["h"] // 2 - 8,
              f"panel {j['x']},{j['y']} {j['w']}x{j['h']}, window {c}")
    typed(dbg, "red heart")
    j = picker(dbg)
    want = j["focused"] or {}
    res.check("'red heart' focuses U+2764, written with U+FE0F", want.get("cp") == "2764" and want.get("vs16") == 1,
              str(want))
    dbg.key(K_ENTER)
    if picker(dbg)["open"]:
        dbg.key(K_ESC)    # nothing focused: Enter typed nothing, and the panel would take the next click
    n_red = poll(lambda: (lambda n: n if n > before + 40 else None)(
        count_yellow(qmp, tmp, (c["x"], c["y"], c["w"], c["h"]), red)), 10, 0.5)
    res.check("the Terminal's cells draw U+2764 U+FE0F as the RED heart", n_red is not None,
              f"{before} red pixels before, {count_yellow(qmp, tmp, (c['x'], c['y'], c['w'], c['h']), red)} after")
    dbg.key("0x15")   # Ctrl+U: the line goes, the shell never runs it
    dbg.close_window("Terminal")


def layout(dbg, *need):
    """Character Map's self-reported rects (content-relative), the newest of each, once all of `need` are in.
    Its widget map is the first page's only, so a page it swaps to is read from the layout log."""
    got = {}
    for ln in dbg.logs("charmap: layout ", clear=False):
        parts = ln.split("charmap: layout ", 1)[1].split()
        try:
            got[parts[0]] = [int(p) for p in parts[1:]]
        except ValueError:
            continue
    return got if all(k in got for k in need) else None


def last_emoji_state(dbg):
    lines = dbg.logs("charmap: layout emoji ", clear=False)
    if not lines:
        return None
    return [int(p) for p in lines[-1].split("charmap: layout emoji ", 1)[1].split()[:2]]


def charmap_part(dbg, qmp, res, tmp):
    dbg.logs("charmap:", clear=True)
    dbg.send("gui spawn /bin/wm/apps/charmap")
    win = poll(lambda: dbg.window_settled(CHARMAP), 15, 0.3)
    if not res.check("Character Map opens", win is not None):
        return
    c = win["content"]
    got = poll(lambda: layout(dbg, "rail", "rail.top", "rail.row_h"), 10, 0.3)
    if not res.check("the rail reports its rows", got is not None):
        return
    rx, ry, rw, rh = got["rail"]
    top, row = got["rail.top"][0], got["rail.row_h"][0]
    dbg.click(c["x"] + rx + rw // 2, c["y"] + ry + top + row + row // 2)
    page = poll(lambda: any("charmap: page emoji" in ln for ln in dbg.logs("charmap: page", clear=False)), 10, 0.3)
    res.check("the rail's second row is Emoji", page)
    got = poll(lambda: layout(dbg, "egrid", "esearch"), 10, 0.3)
    if not res.check("the Emoji page shows its grid", got is not None):
        return
    dbg.settle()
    gx, gy, gw, gh = got["egrid"]
    n_y = count_yellow(qmp, tmp, (c["x"] + gx, c["y"] + gy, gw, gh))
    res.check("Character Map's Emoji page draws in colour", n_y > 400, f"{n_y} yellow pixels")
    st = poll(lambda: last_emoji_state(dbg), 5, 0.3)
    res.check("it opens on a group with emoji in it", st is not None and st[0] > 20, str(st))

    # 6. :name: completion, in the page's own search field.
    sx, sy, sw, sh = got["esearch"]
    dbg.click(c["x"] + sx + sw // 2, c["y"] + sy + sh // 2)
    typed(dbg, ":grinning")
    dbg.key(K_TAB)
    st = poll(lambda: (lambda v: v if v and v[0] == 1 else None)(last_emoji_state(dbg)), 10, 0.3)
    searches = [ln for ln in dbg.logs("charmap: emoji search", clear=False)]
    res.check("':grinning' then Tab completes to the emoji, which the search finds alone",
              st is not None and st[1] == 0, f"state {last_emoji_state(dbg)}, {searches[-2:]}")
    dbg.close_window(CHARMAP)


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "emoji_picker_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("emoji picker: Super+period over Notepad, and Character Map's Emoji page")
    with tempfile.TemporaryDirectory() as tmp:
        try:
            # A clean row, so "starts with it" is about this run.
            dbg.send("sh config unset desktop.emoji.recent")
            notepad_part(dbg, qmp, res, tmp)
            terminal_part(dbg, qmp, res, tmp)
            charmap_part(dbg, qmp, res, tmp)
        finally:
            dbg.close()
    return res.finish("emoji_picker_test")


if __name__ == "__main__":
    sys.exit(main())
