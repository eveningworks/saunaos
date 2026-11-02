#!/usr/bin/env python3
"""Hex Viewer (/bin/wm/apps/hexview: ui/uui_hexview, ui/uui_byteinspect, lib/ubinmap) on a GENERATED ELF.

The file is ubinmap_hostcheck.py's ELF64 (an entry of 0x8000002BC0, an interpreter, a note, three
sections) with 16 KiB of NUMBERED rows after it -- each 16-byte row starts with its own row number --
put into the guest with `vm.py put`, so every offset, value and node is known and no two rows draw
alike. The decoders' exact nodes are ubinmap_hostcheck.py's; this is what only the window can show.

WHAT IT CHECKS, and what a broken version would still pass:

  - it opens the file and names its format (`hexview: opened ... format "ELF64 executable, x86-64"`);
  - the ROWS ARE DRAWN AT THEIR OFFSETS: scrolled six rows by the wheel, the top row's offset column
    is pixel-identical to what row six showed before, and different from what row zero showed (a
    grid that scrolled its state but not its drawing, or drew every row's offset alike, fails one);
  - a selection FILLS BOTH COLUMNS: a byte inside it changes colour in the hex column AND the text
    column, and a byte outside it, sampled the same way, does not;
  - the inspector's values for a known cursor (Int64 and Hex 64 of the entry), the Big segment
    swapping them, and "As an offset" + Go to landing on 0x2BC0;
  - a STRUCTURE NODE click selects exactly that node's bytes (the interpreter's offset and length);
  - Find in hex and in text lands on the match it must, and a query that is not there says so;
  - Copy as hex goes through the CLIPBOARD: pasted back into the find field, it finds the entry
    field it came from;
  - Go to by an absolute and a relative offset;
  - 32 per row moves the text column right and lays 32 bytes a row;
  - Copy as hex on the command bar is GREYED with nothing selected, by its own pixels against the
    same button with a selection; the right-click menu's rows grey the same way;
  - the menu bar, which nothing here touches, is pixel-identical before and after (the control).

The pointer is parked on the status bar before every capture (its sprite alone makes frames differ).
Positive controls (mutate.py): drawing the text column without the selection band reddens the
both-columns check; a tree click that selects nothing reddens the structure check.

    python3 tools/lib/vm.py start
    python3 tools/tests/gui/hexview_test.py
"""

RUNNER = {
    "covers": "Hex Viewer on a generated ELF: rows at their offsets, selection in both columns, inspector, structure click, find, go to, copy, 32 per row, greyed commands",
}

import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results, poll               # noqa: E402
import ubinmap_hostcheck                        # noqa: E402

GUEST = "/tmp/hvtest.elf"
TITLE = "hvtest.elf - Hex Viewer"
K_ENTER, K_ESC, K_BS = "0x0d", "0x1b", "0x08"
K_SHIFT_RIGHT = "0xf78c"
NUMBERED_ROWS = 1024


def build_file():
    data, at = ubinmap_hostcheck.elf64()
    data = data.ljust((len(data) + 15) // 16 * 16, b"\0")
    base_row = len(data) // 16
    body = b"".join(struct.pack(">I", base_row + r) + bytes(((base_row + r) * 7 + k) & 0xFF for k in range(12))
                    for r in range(NUMBERED_ROWS))
    return data + body, at


def put_file(instance, data):
    """Before the console is ours: `vm.py put` shares the debug socket (hlpview_test.py's rule)."""
    with tempfile.NamedTemporaryFile(suffix=".elf", delete=False) as f:
        f.write(data)
        host = f.name
    vm = [sys.executable, toolpath.tool("vm.py"), "--instance", str(instance)]
    ok = subprocess.run(vm + ["put", host, GUEST], capture_output=True).returncode == 0
    os.unlink(host)
    return ok


class View:
    """The app's own reports: the newest `state` line, the layout numbers, the tree's rows."""

    def __init__(self, dbg):
        self.dbg = dbg
        self.lines = []

    def pump(self):
        self.lines += self.dbg.logs("hexview:", clear=True)
        self.lines = self.lines[-3000:]

    def state(self):
        for ln in reversed(self.lines):
            m = re.search(r'hexview: state cursor (\d+) sel (\d+) (\d+) col (\w+) per_row (\d+) top (\d+) tree (-?\d+)'
                          r' path "(.*)"', ln)
            if m:
                return dict(cursor=int(m.group(1)), lo=int(m.group(2)), len=int(m.group(3)), col=m.group(4),
                            per_row=int(m.group(5)), top=int(m.group(6)), tree=int(m.group(7)), path=m.group(8))
        return None

    def num(self, key):
        for ln in reversed(self.lines):
            m = re.search(r"hexview: layout " + re.escape(key) + r" (-?\d+)$", ln)
            if m:
                return int(m.group(1))
        return None

    def text(self, key):
        for ln in reversed(self.lines):
            m = re.search(r"hexview: layout " + re.escape(key) + r" (.*)$", ln)
            if m:
                return m.group(1).strip()
        return None

    def rect(self, key):
        for ln in reversed(self.lines):
            m = re.search(r"hexview: layout " + re.escape(key) + r" (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
            if m:
                return tuple(int(g) for g in m.groups())
        return None

    def tree_row(self, label):
        for ln in reversed(self.lines):
            m = re.search(r'hexview: tree row (\d+) node (\d+) "(.*)"$', ln)
            if m and m.group(3) == label:
                return int(m.group(1))
        return None

    def has(self, pattern):
        return any(re.search(pattern, ln) for ln in self.lines)

    def wait(self, pred, timeout=10.0):
        def look():
            self.dbg.settle()
            self.pump()
            s = self.state()
            return s if s and pred(s) else None
        return poll(look, timeout, step=0.3)

    def wait_log(self, pattern, timeout=10.0):
        def look():
            self.pump()
            return self.has(pattern)
        return poll(look, timeout, step=0.3)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default=None, help="keep the screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "hexview_test")
    res = Results()
    check = res.check
    tmp = args.tmp or tempfile.mkdtemp(prefix="hexview-")
    os.makedirs(tmp, exist_ok=True)
    data, at = build_file()

    put_ok = put_file(args.instance if args.instance is not None else 0, data)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("Hex Viewer on a generated ELF")
    try:
        if check("the test file reached the guest", put_ok):
            run(dbg, qmp, tmp, res, data, at)
    finally:
        for i, w in reversed(list(enumerate(dbg.windows() or []))):
            if "Hex Viewer" in (w.get("title") or ""):
                dbg.send(f"gui close {i}")
        dbg.close()
    return res.finish("hexview_test")


def run(dbg, qmp, tmp, res, data, at):
    from PIL import Image
    check = res.check
    v = View(dbg)
    dbg.logs("hexview:", clear=True)
    dbg.send(f"gui spawn /bin/wm/apps/hexview {GUEST}")
    win = poll(lambda: dbg.window(TITLE), 15, 0.3)
    if not check("the window opens, titled by its file", win is not None, f"windows {[w.get('title') for w in dbg.windows() or []]}"):
        return
    ok = v.wait_log(r'hexview: opened %s size %d format "ELF64 executable, x86-64"' % (GUEST, len(data)))
    check("it opens the file and names its format", ok, "\n".join(ln for ln in v.lines if "opened" in ln)[-300:])
    s = v.wait(lambda s: s["cursor"] == 0)
    cx0, cy0 = win["content"]["x"], win["content"]["y"]
    grid = v.rect("grid")
    if not check("the grid reports where it is", grid is not None and v.num("grid.row_h"), "no layout"):
        return
    gx, gy, gw, gh = grid
    row_h, cw, hex_x, text_x, rows_y = (v.num("grid.row_h"), v.num("grid.cell_w"), v.num("grid.hex_x"),
                                        v.num("grid.text_x"), v.num("grid.rows_y"))
    status = v.rect("menu")   # the menu bar: the control that must not change
    park = (cx0 + gx + gw // 2, cy0 + gy + gh + 6)    # the status bar

    def shot(name):
        dbg.warp_cursor(qmp, *park)
        dbg.settle()
        p = os.path.join(tmp, name)
        qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    def cell(off, text=False, top=0, per_row=16):
        """Screen rect of byte `off` (hex or text column) with row `top` at the top."""
        row, i = off // per_row - top, off % per_row
        v.pump()   # the rows move down while the find strip is open: ask where they are NOW
        y = cy0 + (v.num("grid.rows_y") or rows_y) + row * row_h
        x = cx0 + (text_x + i * cw if text else hex_x + i * 3 * cw + (i // 8) * cw)
        return x, y, cw if text else 3 * cw, row_h

    def crop(im, r):
        x, y, w, h = r
        return im.crop((x, y, x + w, y + h)).tobytes()

    def offset_box(k):
        return (cx0 + gx + cw, cy0 + rows_y + k * row_h, hex_x - gx - 2 * cw, row_h)

    menu_before = shot("open.png")
    im = menu_before

    # --- 1. rows drawn at their offsets: scroll six rows, row 0 must now show what row 6 showed -----------
    row0, row6 = crop(im, offset_box(0)), crop(im, offset_box(6))
    check("two rows' offsets draw differently (the file's rows are numbered)", row0 != row6)
    dbg.warp_cursor(qmp, cx0 + gx + gw // 2, cy0 + gy + gh // 2)
    dbg.wheel(-2)
    s = v.wait(lambda s: s["top"] == 6)
    check("the wheel down scrolls the grid on (3 rows a notch)", s is not None, f"{v.state()}")
    im = shot("scrolled.png")
    check("...and the top row now shows the offset row 6 showed before (drawn at its offset)",
          crop(im, offset_box(0)) == row6 and crop(im, offset_box(0)) != row0)
    dbg.warp_cursor(qmp, cx0 + gx + gw // 2, cy0 + gy + gh // 2)
    dbg.wheel(4)
    v.wait(lambda s: s["top"] == 0)

    # --- 2. the inspector at a known cursor ------------------------------------------------------------
    x, y, w, h = cell(0x18)
    dbg.click(x + w // 2, y + h // 2)
    s = v.wait(lambda s: s["cursor"] == 0x18 and s["len"] == 0)
    check("a click puts the cursor on the byte (0x18), nothing selected", s is not None, f"{v.state()}")
    dbg.settle()
    v.pump()
    check("the inspector reads the entry: Int64 549755825088 (0x8000002BC0)",
          v.text("inspector.int64") == "549755825088", f"{v.text('inspector.int64')}")
    check("...Hex 64 0x0000008000002BC0", v.text("inspector.hex64") == "0x0000008000002BC0", v.text("inspector.hex64"))
    check("the status bar's path says ELF header > Entry", s is not None and s["path"] == "ELF header > Entry",
          f"{v.state()}")
    before_sel = shot("cursor.png")
    toolbar_copy = v.rect("toolbar.button 5")

    # --- 3. Copy as hex greyed with nothing selected; then a selection in both columns ----------------
    for _ in range(7):
        dbg.key(K_SHIFT_RIGHT, settle=False)
    s = v.wait(lambda s: s["lo"] == 0x18 and s["len"] == 8)
    check("Shift+Right selects 0x18-0x1F", s is not None, f"{v.state()}")
    after_sel = shot("selected.png")

    def px(im, r, dx, dy):
        return im.getpixel((r[0] + dx, r[1] + dy))

    inside_h, inside_t = cell(0x1A), cell(0x1C, text=True)
    out_h, out_t = cell(0x30), cell(0x30, text=True)
    changed = lambda r, dx, dy: px(before_sel, r, dx, dy) != px(after_sel, r, dx, dy)   # noqa: E731
    check("the selection fills the HEX column", changed(inside_h, 2, row_h // 2),
          f"{px(before_sel, inside_h, 2, row_h // 2)} -> {px(after_sel, inside_h, 2, row_h // 2)}")
    check("...and the TEXT column in step", changed(inside_t, cw // 2, 1),
          f"{px(before_sel, inside_t, cw // 2, 1)} -> {px(after_sel, inside_t, cw // 2, 1)}")
    check("...and a byte outside it is untouched in both", not changed(out_h, 2, row_h // 2) and not changed(out_t, cw // 2, 1))
    if toolbar_copy:
        tb = (cx0 + toolbar_copy[0], cy0 + toolbar_copy[1], toolbar_copy[2], toolbar_copy[3])
        check("Copy as hex is GREYED with nothing selected and not with a selection (its own pixels)",
              crop(before_sel, tb) != crop(after_sel, tb))
    check("the menu bar, which nothing touched, is unchanged",
          status and crop(menu_before, (cx0 + status[0], cy0 + status[1], status[2], status[3]))
          == crop(after_sel, (cx0 + status[0], cy0 + status[1], status[2], status[3])))

    # --- 4. the right-click menu greys by the selection -------------------------------------------------
    def menu_strengths():
        dbg.settle()
        v.pump()
        rows = {}
        for ln in v.lines[-200:]:
            m = re.search(r"layout ctxmenu\.item 0 (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
            if m:
                rows[int(m.group(1))] = tuple(int(g) for g in m.groups()[1:])
        full = sorted((r for r in rows.values() if r[3] > 12), key=lambda r: r[1])
        if len(full) < 5:
            return None
        im = shot("menu.png")

        def strength(r):
            p = [sum(im.getpixel((xx, yy))) // 3 for yy in range(cy0 + r[1], cy0 + r[1] + r[3], 2)
                 for xx in range(cx0 + r[0], cx0 + r[0] + 100, 2)]
            bg = sorted(p)[len(p) // 2]
            return max(abs(q - bg) for q in p)
        return [strength(r) for r in full]

    x, y, w, h = cell(0x1C)
    v.pump()
    dbg.rclick(x + w // 2, y + h // 2)
    with_sel = poll(menu_strengths, 6, 0.4)
    dbg.key(K_ESC)
    x, y, w, h = cell(0x40)
    dbg.rclick(x + w // 2, y + h // 2)     # outside the selection: moves there, selects nothing
    no_sel = poll(menu_strengths, 6, 0.4)
    dbg.key(K_ESC)
    check("the right-click menu opens with its rows (twice)", with_sel and no_sel, f"{with_sel} / {no_sel}")
    if with_sel and no_sel:
        check("...Copy as hex is greyed with nothing selected, not with a selection, and Select all is "
              "never greyed", no_sel[0] * 1.25 < with_sel[0] and abs(no_sel[-1] - with_sel[-1]) < 40,
              f"copy {with_sel[0]} -> {no_sel[0]}, select all {with_sel[-1]} / {no_sel[-1]}")

    # --- 5. the Big segment and As an offset ---------------------------------------------------------------
    x, y, w, h = cell(0x18)
    dbg.click(x + w // 2, y + h // 2)
    v.wait(lambda s: s["cursor"] == 0x18 and s["len"] == 0)
    seg = v.rect("inspector.endian")
    if check("the inspector reports its byte-order switch", seg is not None):
        dbg.click(cx0 + seg[0] + seg[2] * 3 // 4, cy0 + seg[1] + seg[3] // 2)
        ok = poll(lambda: (dbg.settle(), v.pump(), v.text("inspector.hex64") == "0xC02B000080000000")[2], 6, 0.3)
        check("Big swaps the values: Hex 64 0xC02B000080000000", ok, v.text("inspector.hex64"))
        dbg.click(cx0 + seg[0] + seg[2] // 4, cy0 + seg[1] + seg[3] // 2)
        poll(lambda: (dbg.settle(), v.pump(), v.text("inspector.hex64") == "0x0000008000002BC0")[2], 6, 0.3)
    link = v.rect("inspector.goto")
    if check("the inspector reports its Go to link", link is not None):
        dbg.click(cx0 + link[0] + link[2] // 2, cy0 + link[1] + link[3] // 2)
        s = v.wait(lambda s: s["cursor"] == 0x2BC0)
        check("As an offset + Go to lands on 0x2BC0", s is not None, f"{v.state()}")

    # --- 6. a structure node selects its bytes --------------------------------------------------------------
    v.pump()
    tree = v.rect("structure")
    trow, trow_h = v.tree_row("Interpreter"), v.num("structure.row_h")
    if check("the structure tree lists the interpreter", tree is not None and trow is not None and trow_h,
             f"{tree} {trow} {trow_h}"):
        dbg.click(cx0 + tree[0] + tree[2] // 2, cy0 + tree[1] + trow * trow_h + trow_h // 2)
        s = v.wait(lambda s: s["lo"] == at["interp_at"] and s["len"] == 15)
        check("a click on Interpreter selects exactly its bytes (0x%X, 15)" % at["interp_at"], s is not None,
              f"{v.state()}")

    # --- 7. Find, hex then text ------------------------------------------------------------------------------
    dbg.key("0x06", mods="ctrl")
    for ch in "6C 64 2D 74 6F 79":            # "ld-toy"
        dbg.key(hex(ord(ch)), settle=False)
    ok = v.wait_log(r"hexview: find done, 1 match$", 15)
    s = v.wait(lambda s: s["lo"] == at["interp_at"] + 5 and s["len"] == 6)
    check("Find in hex finds ld-toy where it is", ok and s is not None, f"{v.state()}")
    mode = v.rect("findmode")
    if check("the find strip reports its Hex/Text switch", mode is not None):
        dbg.click(cx0 + mode[0] + mode[2] * 3 // 4, cy0 + mode[1] + mode[3] // 2)
        ok = v.wait_log(r"hexview: find done, 0 matches$", 15)
        check("...the same query as TEXT is not there, and says so", ok)
        fld = v.rect("find.field")
        if fld:
            dbg.click(cx0 + fld[0] + fld[2] - 4, cy0 + fld[1] + fld[3] // 2)
        for _ in range(len("6C 64 2D 74 6F 79")):
            dbg.key(K_BS, settle=False)
        for ch in "toy.so":
            dbg.key(hex(ord(ch)), settle=False)
        s = v.wait(lambda s: s["lo"] == at["interp_at"] + 8 and s["len"] == 6, 15)
        check("Find in text finds toy.so", s is not None, f"{v.state()}")
        # back to hex, for the clipboard round trip below
        dbg.click(cx0 + mode[0] + mode[2] // 4, cy0 + mode[1] + mode[3] // 2)

    # --- 8. Copy as hex through the clipboard ---------------------------------------------------------------
    # A click in the grid takes the keys back from the find field; Ctrl+Home brings row 0 to the top.
    x, y, w, h = cell(0x40)
    dbg.click(x + w // 2, y + h // 2)
    dbg.key("0xf786", mods="ctrl")
    v.wait(lambda s: s["top"] == 0 and s["cursor"] == 0)
    x, y, w, h = cell(0x18)
    dbg.click(x + w // 2, y + h // 2)
    for _ in range(7):
        dbg.key(K_SHIFT_RIGHT, settle=False)
    s = v.wait(lambda s: s["lo"] == 0x18 and s["len"] == 8)
    check("a click in the grid takes the keys back from Find (Ctrl+Home, then Shift+Right select 0x18-0x1F)",
          s is not None, f"{v.state()}")
    dbg.key("0x03", mods="ctrl")
    ok = v.wait_log(r"hexview: copied 8 bytes as hex: ok")
    check("Ctrl+C in the hex column copies the selection as hex", ok, str([ln for ln in v.lines if "cop" in ln][-3:]))
    fld = v.rect("find.field")
    if fld:
        dbg.click(cx0 + fld[0] + fld[2] - 4, cy0 + fld[1] + fld[3] // 2)
        dbg.key("0x01", mods="ctrl")       # select the field's text, so the paste replaces it
        dbg.key("0x16", mods="ctrl")
        s = v.wait(lambda s: s["lo"] == 0x18 and s["len"] == 8 and v.has(r"find done, 1 match$"), 15)
        check("...and pasted into Find, the hex finds the bytes it came from (0x18)", s is not None,
              f"{v.state()} {[ln for ln in v.lines if 'find' in ln][-3:]}")
    dbg.key(K_ESC)

    # --- 9. Go to, absolute and relative -------------------------------------------------------------------
    x, y, w, h = cell(0x20)
    dbg.click(x + w // 2, y + h // 2)
    dbg.key("0x07", mods="ctrl")
    poll(lambda: dbg.window("Go to offset"), 6, 0.3)
    for ch in "0x100":
        dbg.key(hex(ord(ch)), settle=False)
    dbg.key(K_ENTER)
    s = v.wait(lambda s: s["cursor"] == 0x100)
    check("Go to 0x100", s is not None, f"{v.state()}")
    dbg.key("0x07", mods="ctrl")
    poll(lambda: dbg.window("Go to offset"), 6, 0.3)
    for ch in "+16":
        dbg.key(hex(ord(ch)), settle=False)
    dbg.key(K_ENTER)
    s = v.wait(lambda s: s["cursor"] == 0x110)
    check("Go to +16, from there", s is not None, f"{v.state()}")

    # --- 10. 32 per row -------------------------------------------------------------------------------------
    b32 = v.rect("toolbar.button 8")
    if check("the command bar reports 32 per row", b32 is not None):
        dbg.click(cx0 + b32[0] + b32[2] // 2, cy0 + b32[1] + b32[3] // 2)
        s = v.wait(lambda s: s["per_row"] == 32)
        dbg.settle()
        v.pump()
        check("32 per row lays 32 bytes a row and moves the text column right",
              s is not None and (v.num("grid.text_x") or 0) > text_x + 40 * cw,
              f"{v.state()} text_x {text_x} -> {v.num('grid.text_x')}")
        b16 = v.rect("toolbar.button 7")
        if b16:
            dbg.click(cx0 + b16[0] + b16[2] // 2, cy0 + b16[1] + b16[3] // 2)    # back to 16
        v.wait(lambda s: s["per_row"] == 16)
        win2 = dbg.window(TITLE)
        if win2:   # the window grew for 32 a row; where its content is now
            cx0, cy0 = win2["content"]["x"], win2["content"]["y"]

    # --- 11. the file dialogs work INSIDE the sandbox: Save under /home, refused elsewhere, Open ---------
    def save_as(path):
        x, y, w, h = cell(0x18)
        dbg.click(x + w // 2, y + h // 2)
        for _ in range(7):
            dbg.key(K_SHIFT_RIGHT, settle=False)
        v.wait(lambda s: s["lo"] == 0x18 and s["len"] == 8)
        sv = v.rect("inspector.save")
        dbg.click(cx0 + sv[0] + sv[2] // 2, cy0 + sv[1] + sv[3] // 2)
        if not poll(lambda: dbg.window("Save Selection As"), 8, 0.3):
            return False
        dbg.key("0x01", mods="ctrl")         # the suggested name, replaced
        for ch in path:
            dbg.key(hex(ord(ch)), settle=False)
        dbg.key(K_ENTER)
        return True

    dbg.send("sh rm /home/hvsave.bin")
    ok = save_as("/home/hvsave.bin")
    saved = v.wait_log(r"hexview: saved 8 of 8 bytes to /home/hvsave\.bin", 10)
    listing = dbg.send("sh ls -l /home/hvsave.bin") or ""
    check("Save as writes the selection under /home (8 bytes, inside the sandbox)", ok and saved and " 8 " in listing,
          f"{listing.strip()[-120:]}")
    ok = save_as("/tmp/hvsave.bin")
    refused = v.wait_log(r"hexview: save refused for /tmp/hvsave\.bin", 10)
    check("...and is refused outside /home, with the app still running", ok and refused and dbg.window(TITLE) is not None)
    dbg.send("sh rm /home/hvsave.bin")
    dbg.key("0x0f", mods="ctrl")
    if check("File > Open's dialog opens in the sandbox", poll(lambda: dbg.window("Open File"), 8, 0.3) is not None):
        # The name field takes a whole path; an Open dialog starts with the listing focused, so it is
        # clicked first -- where the chooser's own layout log says it is (filedialog_test.py's fd_geom).
        def name_rect():
            for ln in reversed(dbg.logs("filedialog: layout name ", clear=False)):
                try:
                    return tuple(int(p) for p in ln.split()[-4:])
                except ValueError:
                    return None
            return None
        rect, dlg = poll(name_rect, 8, 0.3), dbg.window("Open File")
        if rect and dlg:
            dbg.click(dlg["content"]["x"] + rect[0] + rect[2] // 2, dlg["content"]["y"] + rect[1] + rect[3] // 2)
        for ch in "/usr/share/fonts/LICENSE-DejaVu.txt":
            dbg.key(hex(ord(ch)), settle=False)
        dbg.key(K_ENTER)
        ok = v.wait_log(r'hexview: opened /usr/share/fonts/LICENSE-DejaVu\.txt size \d+ format ""', 10)
        check("...and opens the file named in it, a text file with no structure known", ok)


if __name__ == "__main__":
    sys.exit(main())
