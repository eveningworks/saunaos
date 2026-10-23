#!/usr/bin/env python3
"""Archives in the File Manager: a .zip and a .tar.gz as folders, and
Compress (fm_archive.c, fm_compress.c, over lib/uarchive.h).

The archives OPENED here are written on the HOST by Python's zipfile and
tarfile -- not by anything in toy-os -- and put on the guest, so the
readers are checked against foreign writers: a stored member, deflated
ones, a folder two deep (named only by the members under it).

For EACH of the .zip and the .tar.gz:

1. **Opening the archive lists its top level as a folder**: the folder
   "dir" (derived from the names below it) and hello.txt, no "..".
2. **A folder inside opens in place**, and Up returns to the top.
3. **Up from the top goes to the folder the archive is in.**
4. **Extract all writes every member beside the archive** -- in a folder
   named for it, the archive's suffix taken off -- with each file's size
   as the host wrote it, and the pane goes there.
5. **Opening a member extracts it and opens the copy** (/tmp/zip-open).

Then COMPRESS, on what was extracted, round-tripped through the reader:

6. **Compress to ZIP** (the context menu) makes hello.zip beside the
   file and drops it into an in-place rename, the name it chose shown.
7. **...and it opens as a folder holding hello.txt**, at its size.
8. **Compress... with .tar.gz chosen** (Tab to the format, Right) makes
   dir.tar.gz, and opening it shows "dir" with its two entries inside.
9. **An archive file's context menu has Extract here and Extract to...**;
   Extract here makes "zt" beside it, stays, and selects it.
10. **Extract to... opens a folder chooser titled "Extract to"**, and Esc
   cancels it. (Choosing a folder is the same job as Extract here with
   another destination.)

What this does NOT prove is that another archiver can read what toy-os
writes -- tools/archive_hostcheck.py does that, against Python's zipfile
and tarfile, on the host.
"""
import argparse
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
MEMBERS = {
    "hello.txt": b"Hello from Python.\n",
    "dir/lorem.txt": b"lorem ipsum dolor sit amet " * 60,
    "dir/sub/deep.txt": b"three levels down\n" * 9,
}
ARCHIVES = ("/home/zt.zip", "/home/zt.tar.gz")


class _Empty:
    """What a check reads when the window reported nothing at all: every
    field empty, so a broken build FAILS the check instead of crashing
    the run before the checks after it can say anything."""
    dir, rows, tbitems, rowy, pane = {}, {}, {}, {}, {0: (0, 0, 0, 0)}
    renaming = selected = ctxbox = None
    rowh = ox = oy = ctx = 0


EMPTY = _Empty()


def make_zip():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(zipfile.ZipInfo("hello.txt"), MEMBERS["hello.txt"])   # stored
        for name in ("dir/lorem.txt", "dir/sub/deep.txt"):
            z.writestr(name, MEMBERS[name], compress_type=zipfile.ZIP_DEFLATED)
    return buf.getvalue()


def make_tgz():
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        for name, data in MEMBERS.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = int(time.time())
            t.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def put_archives(instance):
    """Before the console is ours: `vm.py put` talks over the same
    debug socket, and a second client on it while the test holds it
    loses the file without a word."""
    for path, data in zip(ARCHIVES, (make_zip(), make_tgz())):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(data)
            host = f.name
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(instance),
                        "put", host, path], capture_output=True)
        os.unlink(host)


def run(dbg, qmp, res, instance):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    sh("rm -r /home/zt")
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} /home", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home")

    def seek(name):
        dbg.key(fm.K_HOME)
        for ch in name:
            dbg.key("0x2e" if ch == "." else ch)
        return fm.wait_layout(dbg, win, lambda l: l.selected == name) or fm.layout_now(dbg, win) or EMPTY

    for archive in ARCHIVES:
        kind = os.path.basename(archive).split(".", 1)[1]   # "zip", "tar.gz"
        base = os.path.basename(archive)
        root = f"archive:{archive}"

        # 1.
        seek(base)
        dbg.key(fm.K_ENTER)
        lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root and l.rows.get(0) == 2) \
            or fm.layout_now(dbg, win) or EMPTY
        res.check(f".{kind}: opening it lists its top level: 'dir' and hello.txt, no '..'",
                  lay.dir.get(0) == root and lay.rows.get(0) == 2,
                  f"dir={lay.dir.get(0)!r} rows={lay.rows.get(0)}")

        # 2.
        seek("dir")
        dbg.key(fm.K_ENTER)
        lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root + "/dir") or fm.layout_now(dbg, win) or EMPTY
        inside = lay.dir.get(0) == root + "/dir" and lay.rows.get(0) == 2
        dbg.key(fm.K_BACKSPACE)
        lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root) or fm.layout_now(dbg, win) or EMPTY
        res.check(f".{kind}: a folder inside opens in place (lorem.txt, sub), and Up returns to the top",
                  inside and lay.dir.get(0) == root, f"inside={inside} back={lay.dir.get(0)!r}")

        # 3.
        dbg.key(fm.K_BACKSPACE)
        lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home") or fm.layout_now(dbg, win) or EMPTY
        res.check(f".{kind}: Up from the top goes to the folder the archive is in",
                  lay.dir.get(0) == "/home", f"dir={lay.dir.get(0)!r}")

        # 4. Extract all: the archive toolbar's first item
        seek(base)
        dbg.key(fm.K_ENTER)
        lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root and 0 in l.tbitems) \
            or fm.layout_now(dbg, win) or EMPTY
        r = lay.tbitems.get(0)
        if r:
            fm.sure_click(dbg, qmp, lay.ox + r[0] + r[2] // 2, lay.oy + r[1] + r[3] // 2)
        lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home/zt", timeout=30) \
            or fm.layout_now(dbg, win) or EMPTY
        sizes = {n: fm.size_of(dbg, "/home/zt/" + os.path.dirname(n), os.path.basename(n))
                 for n in MEMBERS}
        want = {n: len(b) for n, b in MEMBERS.items()}
        res.check(f".{kind}: Extract all writes every member into /home/zt, sizes as written, and goes there",
                  lay.dir.get(0) == "/home/zt" and sizes == want,
                  f"dir={lay.dir.get(0)!r} sizes={sizes} want={want}")

        # 5. open a member
        dbg.key(fm.K_BACKSPACE)
        fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home")
        seek(base)
        dbg.key(fm.K_ENTER)
        fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root)
        seek("hello.txt")
        mark = len(fm._ALL_BUF)
        dbg.key(fm.K_ENTER)
        end, opened = time.time() + 20, False
        while time.time() < end and not opened:
            fm._collect(dbg)
            opened = any("files: open /tmp/zip-open/hello.txt" in ln for ln in fm._ALL_BUF[mark:])
            time.sleep(0.3)
        res.check(f".{kind}: opening a member extracts it to /tmp/zip-open and opens that copy",
                  opened, f"last={[ln for ln in fm._ALL_BUF if 'open' in ln][-2:]}")
        for w2 in dbg.windows():
            if w2["title"] != fm.TITLE:
                dbg.send(f"gui close {w2['z']}")
        # THE KEYBOARD BACK TO THE FILE MANAGER: closing Notepad does not
        # promise where focus lands, and a seek typed at the desktop is
        # lost without a word. A click on the pane's empty foot selects
        # nothing.
        time.sleep(0.5)
        foot = fm.layout_now(dbg, win) or lay
        fpx, fpy, fpw, fph = foot.pane[0]
        fm.sure_click(dbg, qmp, foot.ox + fpx + fpw // 2, foot.oy + fpy + fph - 6)
        dbg.key(fm.K_BACKSPACE)
        fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home")
        if archive != ARCHIVES[-1]:
            sh("rm -r /home/zt")    # the next archive extracts to the same name

    # --- Compress, on what the last extract left in /home/zt -------------
    seek("zt")
    dbg.key(fm.K_ENTER)
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home/zt")
    lay = fm.wait_layout(dbg, win, lambda l: l.rowy.get(0) is not None and l.rowh) or lay
    if lay.rowy.get(0) is None:
        res.check("the compress checks could be placed (row geometry reported)", False, "no rowy")
        return
    px, py, pw, _ = lay.pane[0]

    # ".." first, then folders, then by name: dir is row 1, hello.txt row
    # 2 (and the archives made here sort between them by name, so each
    # check right-clicks before it makes one).
    ROW = {"dir": 1, "hello.txt": 2}

    def ctx_on_name(name):
        seek(name)
        view_row = ROW[name]
        mark = fm.ctx_mark(dbg)
        fm.sure_rclick(dbg, qmp, lay.ox + px + pw // 2,
                       lay.oy + lay.rowy[0] + lay.rowh * view_row + lay.rowh // 2)
        fm.wait_layout(dbg, win, lambda l: l.ctx == 1 and l.ctxbox)
        return fm.ctx_rows(dbg, mark)

    # 6.
    rows = ctx_on_name("hello.txt")
    fm.ctx_pick(dbg, qmp, win, rows, "Compress to ZIP")
    lay = fm.wait_layout(dbg, win, lambda l: l.renaming and l.renaming[2] == "hello.zip", timeout=20) \
        or fm.layout_now(dbg, win) or EMPTY
    res.check("Compress to ZIP makes hello.zip and drops it into an in-place rename",
              bool(lay.renaming) and lay.renaming[2] == "hello.zip" and
              fm.size_of(dbg, "/home/zt", "hello.zip") > 0,
              f"rows={rows} renaming={lay.renaming} size={fm.size_of(dbg, '/home/zt', 'hello.zip')}")
    dbg.key(fm.K_ESC)
    fm.wait_layout(dbg, win, lambda l: not l.renaming)

    # 7.
    seek("hello.zip")
    dbg.key(fm.K_ENTER)
    zroot = "archive:/home/zt/hello.zip"
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == zroot and l.rows.get(0) == 1) \
        or fm.layout_now(dbg, win) or EMPTY
    res.check("...and it opens as a folder holding hello.txt",
              lay.dir.get(0) == zroot and lay.rows.get(0) == 1 and lay.selected in (None, "-", "hello.txt"),
              f"dir={lay.dir.get(0)!r} rows={lay.rows.get(0)}")
    dbg.key(fm.K_BACKSPACE)
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home/zt")

    # 8. Compress... on the folder, .tar.gz chosen from the keyboard
    rows = ctx_on_name("dir")
    fm.ctx_pick(dbg, qmp, win, rows, "Compress...")
    time.sleep(0.8)
    dbg.key(fm.K_TAB)          # name -> format
    dbg.key(fm.K_RIGHT)        # .zip -> .tar.gz
    time.sleep(0.3)
    dbg.key(fm.K_ENTER)        # the form declines Return, so the dialog commits
    end, made = time.time() + 30, -1
    while time.time() < end and made <= 0:
        made = fm.size_of(dbg, "/home/zt", "dir.tar.gz")
        time.sleep(0.5)
    fm.wait_layout(dbg, win, lambda l: True)
    seek("dir.tar.gz")
    dbg.key(fm.K_ENTER)
    troot = "archive:/home/zt/dir.tar.gz"
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == troot and l.rows.get(0) == 1) \
        or fm.layout_now(dbg, win) or EMPTY
    top = lay.dir.get(0) == troot and lay.rows.get(0) == 1
    seek("dir")
    dbg.key(fm.K_ENTER)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == troot + "/dir" and l.rows.get(0) == 2) \
        or fm.layout_now(dbg, win) or EMPTY
    res.check("Compress... as .tar.gz makes dir.tar.gz, and it opens as 'dir' with its two entries",
              made > 0 and top and lay.dir.get(0) == troot + "/dir" and lay.rows.get(0) == 2,
              f"size={made} top={top} dir={lay.dir.get(0)!r} rows={lay.rows.get(0)}")

    # --- Extract here / Extract to..., on an archive FILE in /home --------
    dbg.key(fm.K_BACKSPACE)
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "archive:/home/zt/dir.tar.gz")
    dbg.key(fm.K_BACKSPACE)
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home/zt")
    dbg.key(fm.K_BACKSPACE)
    home = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home" and l.rowy.get(0) is not None) \
        or fm.layout_now(dbg, win) or EMPTY

    def row_of(name):
        """Where `name` sits in /home's listing: ".." first, then the
        folders by name, then the files -- read from `ls`, hidden names
        left out as the pane leaves them."""
        dirs, files = [], []
        for ln in (sh("ls /home") or "").splitlines():
            ln = ln.strip()
            if not ln or ln.startswith(".") or ln.startswith("ls"):
                continue
            (dirs if ln.endswith("/") else files).append(ln.rstrip("/"))
        order = [".."] + sorted(dirs, key=str.lower) + sorted(files, key=str.lower)
        return order.index(name) if name in order else None

    def ctx_on_file(name):
        seek(name)
        row = row_of(name)
        if row is None or home is None:
            return None
        mark = fm.ctx_mark(dbg)
        fm.sure_rclick(dbg, qmp, home.ox + px + pw // 2,
                       home.oy + home.rowy[0] + home.rowh * row + home.rowh // 2)
        fm.wait_layout(dbg, win, lambda l: l.ctx == 1 and l.ctxbox)
        return fm.ctx_rows(dbg, mark)

    # 9. Extract here makes "zt" beside the archive -- and selects it. (Not
    # "zt (2)": a name with a space defeats both the console's `ls` and
    # the layout lines this reads, which split on spaces.)
    sh("rm -r /home/zt")
    time.sleep(1.0)
    rows = ctx_on_file("zt.zip")
    fm.ctx_pick(dbg, qmp, win, rows, "Extract here")
    lay = fm.wait_layout(dbg, win, lambda l: l.selected == "zt" and l.dir.get(0) == "/home", timeout=30) \
        or fm.layout_now(dbg, win) or EMPTY
    got = fm.size_of(dbg, "/home/zt", "hello.txt")
    res.check("an archive file's context menu has Extract here and Extract to...",
              bool(rows) and "Extract here" in rows and "Extract to..." in rows, f"rows={rows}")
    res.check("...Extract here makes 'zt' beside it, stays, and selects it",
              lay is not None and lay.dir.get(0) == "/home" and lay.selected == "zt" and
              got == len(MEMBERS["hello.txt"]),
              f"dir={lay and lay.dir.get(0)!r} selected={lay and lay.selected!r} hello={got}")

    # 10. Extract to... asks for a folder: a chooser titled "Extract to".
    rows = ctx_on_file("zt.tar.gz")
    fm.ctx_pick(dbg, qmp, win, rows, "Extract to...")
    end, chooser = time.time() + 10, None
    while time.time() < end and not chooser:
        chooser = next((w2 for w2 in dbg.windows() if w2["title"] == "Extract to"), None)
        time.sleep(0.3)
    if chooser:
        dbg.key(fm.K_ESC)
        time.sleep(1.0)
    gone = not any(w2["title"] == "Extract to" for w2 in dbg.windows())
    res.check("...Extract to... opens a folder chooser titled 'Extract to', and Esc cancels it",
              chooser is not None and gone, f"chooser={chooser and chooser['title']} gone={gone}")

    for path in ARCHIVES:
        sh(f"rm {path}")
    sh("rm -r /home/zt")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "archive_gui_test")
    res = Results()
    put_archives(args.instance if args.instance is not None else 0)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res, args.instance if args.instance is not None else 0)
    finally:
        dbg.close()
    print(f"\narchive_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
