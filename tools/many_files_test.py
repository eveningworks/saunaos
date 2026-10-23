#!/usr/bin/env python3
"""tools/many_files_test.py -- apps given more than their old tables held.

WHAT THIS IS
------------
Each of these kept its list in a fixed table and dropped the rest
without a word; each now grows its list to fit. A seed script fills a
folder past every old cap, and the app's own count line must report ALL
of it:

  Terminal        12 tabs           (was 8 a window, 8 ptys kernel-wide)
  Image Viewer    80 images         (was 64)
  Video           210 videos        (was 200)
  Audio Player    140 tracks        (was 128)
  Crash Reports   70+ reports       (was 64)
  Help            300 more pages in 20 more folders and 40 more
                  categories        (was 256 pages, 16 folders, 32
                                     categories)
  Notepad Find    3120 matches      (was 2048)
  Terminal find   3120+ matches     (was 512)
  File Manager    431 search hits   (was one SYS_LISTDIR batch, 256)
                  130+ tree rows    (was 96)

and Image Viewer must survive a Reload of a folder that has gone, where
its list is a NULL array and a stale current index reads through it.

The seed files only have to PROBE as their type: a video is the first
8 KiB of a stock one, a crash report is the header ucrash_load() checks.
What is counted is the listing, not whether each file plays.

Its positive control is a build before the change: every count stops at
its old cap.

    python3 tools/vm.py start && python3 tools/many_files_test.py
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                     # noqa: E402

N_IMG, N_VID, N_MUS, N_CRASH = 80, 210, 140, 70
HELP_DIRS, HELP_PAGES, HELP_CATS = 20, 15, 40
N_TABS = 12
AB_LINES, AB_PER_LINE = 120, 26          # "ab " x 26: 78 columns, under 80
N_DIRS = 120
_res = Results()
check = _res.check

VID_SRC = os.path.join(REPO, "data/usr/share/videos/countdown.mpg")
# ucrash_load() wants the header and a `program:` line; nothing else.
CRASH = "toy-os crash report\npid: 4242\nprogram: /tests/many_files\nfault: page fault\n"


def seed_script():
    loop = "i=0; while [ $i -lt {n} ]; do {body}; i=$((i+1)); done"
    lines = [
        "rm -r /home/many",
        "mkdir /var/crash",   # made by the first crash; a fresh disk has none
        "mkdir /home/many", "mkdir /home/many/img", "mkdir /home/many/vid", "mkdir /home/many/mus",
        loop.format(n=N_IMG, body="cp /usr/share/icons/about.qoi /home/many/img/i$i.qoi"),
        loop.format(n=N_VID, body="cp /tmp/many_vid.mpg /home/many/vid/v$i.mpg"),
        loop.format(n=N_MUS, body="cp /usr/share/music/first-boot.mid /home/many/mus/m$i.mid"),
        loop.format(n=N_CRASH, body="cp /tmp/many.crash /var/crash/many$i.crash"),
        loop.format(n=AB_LINES, body="printf '%s\\n' '" + "ab " * (AB_PER_LINE - 1) + "ab' >> /home/many/ab.txt"),
        "mkdir /home/many/dirs",
        loop.format(n=N_DIRS, body="mkdir /home/many/dirs/d$i"),
        # A page per (folder, page); its category cycles through HELP_CATS.
        f"d=0; while [ $d -lt {HELP_DIRS} ]; do mkdir /usr/share/doc/many$d; p=0; "
        f"while [ $p -lt {HELP_PAGES} ]; do "
        f"printf '# Many %s-%s\\n\\n**Category:** Many C%s\\n' $d $p $(( (d*{HELP_PAGES}+p) % {HELP_CATS} )) "
        f"> /usr/share/doc/many$d/p$p.md; p=$((p+1)); done; d=$((d+1)); done",
        "echo many-seed-done",
    ]
    return "\n".join(lines) + "\n"


def vm(instance, *argv, timeout=None):
    cmd = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(instance)]
    if timeout:
        cmd += ["--timeout", str(timeout)]
    return subprocess.run(cmd + list(argv), cwd=REPO, capture_output=True, text=True)


def seed(instance):
    """Before the console is ours: `vm.py` talks over the same socket."""
    with open(VID_SRC, "rb") as f:
        head = f.read(8192)
    tmp = tempfile.mkdtemp(prefix="many_files_")
    files = {"many_vid.mpg": head, "many.crash": CRASH.encode(), "many_seed.sh": seed_script().encode()}
    for name, data in files.items():
        with open(os.path.join(tmp, name), "wb") as f:
            f.write(data)
        vm(instance, "put", os.path.join(tmp, name), f"/tmp/{name}")
    r = vm(instance, "exec", "dash /tmp/many_seed.sh", timeout=900)
    return "many-seed-done" in (r.stdout + r.stderr)


def count_line(dbg, prefix, pattern, timeout=60.0):
    """The first number `pattern` captures from a log line holding `prefix`."""
    end = time.time() + timeout
    while time.time() < end:
        for ln in reversed(dbg.logs(prefix, clear=False)):
            m = re.search(pattern, ln[ln.index(prefix):])
            if m:
                return tuple(int(g) for g in m.groups())
        time.sleep(0.5)
    return None


def close(dbg, title):
    """Alt+F4 on the app just opened -- the focused window -- once it is
    up; a title may have grown a file name in front (`x - Image Viewer`)."""
    if any(w["title"] == title or w["title"].endswith(" - " + title) for w in dbg.windows()):
        dbg.send("gui key 0xf794 alt")
        dbg.settle()
        time.sleep(0.5)


def terminal_tabs(dbg, qmp):
    dbg.logs("uterm: layout cursor", clear=True)
    if not dbg.spawn("/bin/wm/apps/uterm", "Terminal"):
        check("a Terminal opens", False, "no window")
        return
    for _ in range(N_TABS - 1):
        qmp.combo(["ctrl", "shift", "t"])
        time.sleep(0.6)
    end, tabs = time.time() + 30, 0
    while time.time() < end and tabs < N_TABS:
        dbg.settle()
        for ln in reversed(dbg.logs("uterm: layout cursor", clear=False)):
            parts = ln.split()
            if "tabs" in parts:
                tabs = int(parts[parts.index("tabs") + 1])
                break
        time.sleep(0.5)
    check(f"the Terminal holds {N_TABS} tabs", tabs == N_TABS, f"tabs {tabs}")
    shells = sum(1 for ln in (dbg.send("sh ps") or "").splitlines() if ln.rstrip().endswith("tosh"))
    check("...each with its own shell on its own pty", shells >= N_TABS, f"{shells} tosh running")
    # Not Alt+F4: closing twelve tabs asks first. Its shells get SIGHUP.
    pids = [int(ln.split()[0]) for ln in (dbg.send("sh ps") or "").splitlines()
            if ln.rstrip().endswith("uterm") and ln.split()[0].isdigit()]
    if pids:
        dbg.send(f"sh kill {max(pids)}")
        dbg.settle()


def app_count(dbg, spawn, title, prefix, pattern, want, what):
    dbg.logs(prefix, clear=True)
    dbg.spawn(spawn)   # the count line is the wait, not the title
    got = count_line(dbg, prefix, pattern)
    n = got[0] if got else None
    check(f"{what}: all {want} listed", n == want, f"{prefix} reported {n}")
    close(dbg, title)


def run(dbg, qmp):
    terminal_tabs(dbg, qmp)
    app_count(dbg, "/bin/wm/apps/imgview /home/many/img/i0.qoi", "Image Viewer",
              "imgview: listing /home/many/img", r"(\d+) image\(s\)", N_IMG, "Image Viewer")
    app_count(dbg, "/bin/wm/apps/video /home/many/vid", "Video Player",
              "video: listing /home/many/vid", r"(\d+) video\(s\)", N_VID, "Video")
    app_count(dbg, "/bin/wm/apps/player /home/many/mus", "Audio Player",
              "player: listing /home/many/mus", r"(\d+) track\(s\)", N_MUS, "Audio Player")

    crashes = [ln.split()[-1] for ln in (dbg.send("sh ls /var/crash") or "").splitlines()
               if ln.rstrip().endswith(".crash")]
    app_count(dbg, "/bin/wm/apps/crashreports", "Crash Reports",
              "crashreports:", r"crashreports: (\d+) report\(s\)", len(crashes),
              f"Crash Reports ({len(crashes)} in /var/crash)")

    dbg.logs("help:", clear=True)
    dbg.spawn("/bin/wm/apps/help")
    got = count_line(dbg, "help:", r"help: (\d+) pages in (\d+) categories")
    pages, cats = got if got else (0, 0)
    check(f"Help lists the {HELP_DIRS * HELP_PAGES} seeded pages on top of the manual",
          pages >= HELP_DIRS * HELP_PAGES + 1, f"{pages} pages")
    check(f"...in their {HELP_CATS} categories", cats >= HELP_CATS + 1, f"{cats} categories")
    close(dbg, "Help")


def layout_num(dbg, prefix, field, pred, timeout=30.0):
    """`field`'s number on the newest `prefix` log line, once `pred` holds."""
    end, v = time.time() + timeout, None
    while time.time() < end:
        dbg.settle()
        for ln in reversed(dbg.logs(prefix, clear=False)):
            parts = ln[ln.index(prefix):].split()
            if field in parts and parts.index(field) + 1 < len(parts):
                v = int(parts[parts.index(field) + 1])
                break
        if v is not None and pred(v):
            return v
        time.sleep(0.5)
    return v


def type_keys(dbg, text):
    for ch in text:
        dbg.key({" ": "0x20", ".": "0x2e", "/": "0x2f"}.get(ch, ch))
    dbg.settle()


def finds(dbg, qmp):
    want = AB_LINES * AB_PER_LINE
    # Notepad: Ctrl+F, "ab".
    dbg.logs("notepad: find", clear=True)
    dbg.spawn("/bin/wm/apps/notepad /home/many/ab.txt")
    time.sleep(2.0)
    dbg.key("0x06")
    type_keys(dbg, "ab")
    got = count_line(dbg, 'notepad: find "ab"', r'"ab" (\d+) matches')
    n = got[0] if got else None
    check(f"Notepad's Find marks all {want} matches", n == want, f"find {n}")
    close(dbg, "Notepad")

    # Terminal: the file on screen and in its scrollback, then Ctrl+Shift+F.
    dbg.logs("uterm: layout cursor", clear=True)
    if dbg.spawn("/bin/wm/apps/uterm", "Terminal"):
        type_keys(dbg, "cat /home/many/ab.txt")
        dbg.key("0x0d")
        time.sleep(3.0)
        dbg.send("gui key 0x06 shift")
        time.sleep(0.6)
        type_keys(dbg, "ab")
        n = layout_num(dbg, "uterm: layout cursor", "hits", lambda v: v >= want)
        check(f"the Terminal's find counts all {want}+ matches", (n or 0) >= want, f"hits {n}")
        pids = [int(ln.split()[0]) for ln in (dbg.send("sh ps") or "").splitlines()
                if ln.rstrip().endswith("uterm") and ln.split()[0].isdigit()]
        if pids:
            dbg.send(f"sh kill {max(pids)}")
            dbg.settle()


def file_manager(dbg):
    conf = "/etc/files.conf"
    # The tree: opening a folder under dirs/ opens dirs/, and all its rows show.
    dbg.write_lines(conf, ["panes=1", "tree=1", "left_view=details", "details_pane=0"])
    dbg.logs("files: layout view", clear=True)
    dbg.spawn("/bin/wm/apps/files /home/many/dirs/d0")
    n = layout_num(dbg, "files: layout view", "tree", lambda v: v >= 0, timeout=5.0)
    rows = None
    end = time.time() + 30
    while time.time() < end:
        dbg.settle()
        for ln in reversed(dbg.logs("files: layout view", clear=False)):
            p = ln[ln.index("files: layout view"):].split()
            if "tree" in p:
                rows = int(p[p.index("tree") + 2])
                break
        if rows is not None and rows >= N_DIRS:
            break
        time.sleep(0.5)
    check(f"the folder tree shows all {N_DIRS} folders under dirs/", (rows or 0) > N_DIRS,
          f"{rows} rows (tree {n})")
    close(dbg, "File Manager")

    # The search: every name with a dot under /home/many.
    want = N_IMG + N_VID + N_MUS + 1
    dbg.write_lines(conf, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    dbg.logs("files: search done", clear=True)
    dbg.spawn("/bin/wm/apps/files /home/many")
    time.sleep(2.0)
    dbg.key("0x06")
    type_keys(dbg, ".")
    dbg.key("0x0d")
    got = count_line(dbg, "files: search done", r"(\d+) found", timeout=90.0)
    n = got[0] if got else None
    check(f"a search lists all {want} matches", n == want, f"found {n}")
    close(dbg, "File Manager")
    dbg.send(f"sh rm {conf}")


def pids_of(dbg, name):
    return [int(ln.split()[0]) for ln in (dbg.send("sh ps") or "").splitlines()
            if ln.rstrip().endswith(name) and ln.split()[0].isdigit()]


def imgview_folder_gone(dbg):
    """Reload on a folder that is gone: the list is empty and its array
    NULL, so a current index left over from before reads through NULL --
    Properties reads the current entry on every paint."""
    dbg.logs("imgview:", clear=True)
    dbg.spawn("/bin/wm/apps/imgview /home/many/img/i5.qoi")
    count_line(dbg, "imgview: listing /home/many/img", r"(\d+) image")
    time.sleep(1.5)
    dbg.key("i")                      # Properties
    dbg.settle()
    before = pids_of(dbg, "imgview")
    dbg.send("sh rm -r /home/many/img")
    dbg.key("0xf79b")                 # F5, Reload
    time.sleep(3.0)
    dbg.settle()
    after = pids_of(dbg, "imgview")
    crashed = [ln for ln in (dbg.send("sh ls /var/crash") or "").splitlines() if "imgview" in ln]
    check("Image Viewer survives a Reload of a folder that is gone",
          bool(before) and after == before and not crashed,
          f"pids {before} -> {after}, crash reports {crashed}")
    for pid in after:
        dbg.send(f"sh kill {pid}")


def cleanup(dbg):
    dbg.send("sh rm -r /home/many")
    for d in range(HELP_DIRS):
        dbg.send(f"sh rm -r /usr/share/doc/many{d}")
    for i in range(N_CRASH):
        dbg.send(f"sh rm /var/crash/many{i}.crash")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--keep", action="store_true", help="leave the seeded files in the guest")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "many_files_test")
    inst = args.instance if args.instance is not None else 0
    print("apps past their old fixed tables")
    seeded = seed(inst)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        if check("the seed script ran to its end", seeded, "" if seeded else "no many-seed-done"):
            run(dbg, qmp)
            finds(dbg, qmp)
            file_manager(dbg)
            imgview_folder_gone(dbg)   # last: it deletes img/
        if not args.keep:
            cleanup(dbg)
    finally:
        dbg.close()
    print(f"\nmany_files_test: {len(_res.passes)} passed, {len(_res.fails)} failed")
    for f in _res.fails:
        print(f"  FAILED: {f}")
    return 1 if _res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
