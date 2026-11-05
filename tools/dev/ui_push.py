#!/usr/bin/env python3
"""Send an edited window file (.ui) to a running guest and restart its program.

    tools/t ui_push calculator              # userland/apps/*/calculator/calculator.ui
    tools/t ui_push path/to/x.ui --instance 2
    tools/t ui_push themeedit --no-restart  # copy only

A .ui file is read once, when its program starts (docs/ui-files-design.md),
so seeing an edit is: copy it to /usr/share/ui/, close the program's
windows, wait until they are GONE, start it again. The wait is the trap --
a single-instance program spawned while its old window is still up only
raises that window, built from the old file, and the edit looks like it
did nothing. Prints the builder's own line for the new start: its window
and widget counts and the time, or the file's first error with its line.

The guest is a `vm.py start` one (the file goes over vm.py's TFTP forward).
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole  # noqa: E402

REPO = toolpath.REPO
GUEST_DIR = "/usr/share/ui"   # UUI_BUILDER_DIR, userland/libs/ui/uui_builder.h


def find_ui(arg):
    """(the .ui file, the program's name). A name is looked up beside the
    program's source, where the build seeds it from."""
    if arg.endswith(".ui"):
        if not os.path.isfile(arg):
            sys.exit(f"ui_push: no file {arg}")
        return arg, os.path.basename(arg)[:-3]
    hits = glob.glob(os.path.join(REPO, "userland/apps/*/*/" + arg + ".ui"))
    if not hits:
        sys.exit(f"ui_push: no userland/apps/*/*/{arg}.ui -- pass the file's path")
    return hits[0], arg


def guest_binary(name):
    """/bin/wm/<class>/<dir>, the Makefile's mapping for userland/apps/<class>/<dir>/,
    or the manifest's INSTALL_AS -- from the program's source folder, found
    by the file's name, so an edited copy kept elsewhere still finds it."""
    hits = glob.glob(os.path.join(REPO, "userland/apps/*/*/" + name + ".ui"))
    if not hits:
        sys.exit(f"ui_push: no program has a {name}.ui in the tree -- say which with --bin")
    d = os.path.dirname(hits[0])
    mk = os.path.join(d, "component.mk")
    if os.path.isfile(mk):
        m = re.search(r"^INSTALL_AS\s*=\s*(\S+)", open(mk).read(), re.M)
        if m:
            return m.group(1)
    cls = os.path.basename(os.path.dirname(d))
    return f"/bin/wm/{cls}/{os.path.basename(d)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ui", help="a program's name (calculator) or a .ui file's path")
    ap.add_argument("--instance", type=int, default=0, metavar="N",
                    help="the vm.py slot the guest runs in (default 0)")
    ap.add_argument("--bin", help="the program to start (default: from the source's folder)")
    ap.add_argument("--no-restart", action="store_true", help="copy the file and stop there")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    src, name = find_ui(args.ui)
    dst = f"{GUEST_DIR}/{name}.ui"
    # vm.py put says how it went on stderr; its exit code is what counts.
    rc = subprocess.run([sys.executable, os.path.join(REPO, "tools/lib/vm.py"),
                         "--instance", str(args.instance), "put", src, dst]).returncode
    if rc:
        return rc
    print(f"ui_push: {src} -> {dst}")
    if args.no_restart:
        return 0

    sock = ".vm.serial" if args.instance == 0 else f".vm.{args.instance}.serial"
    dbg = DebugConsole(os.path.join(REPO, sock))
    binary = args.bin or guest_binary(name)
    prog = os.path.basename(binary)
    pids = {p["pid"] for p in dbg.processes_named(prog) if p["name"] == prog}
    titles = [w["title"] for w in dbg.windows() if w.get("client_pid") in pids]
    for t in titles:
        # close_window() asks, as the close button does; an app may refuse.
        if not dbg.close_window(t, timeout=args.timeout) and not dbg.wait_gone(t, args.timeout):
            print(f"ui_push: '{t}' did not close (did it ask something?) -- not restarted")
            return 1

    dbg.logs("uui_builder:")                      # drop older starts' lines
    dbg.spawn(binary)
    deadline = time.time() + args.timeout
    line = None
    while line is None and time.time() < deadline:
        hits = [l for l in dbg.logs("uui_builder:") if f"{name}.ui" in l]
        line = hits[-1].strip() if hits else None
        if line is None:
            time.sleep(0.3)
    if line is None:
        print(f"ui_push: {binary} started but never logged reading {name}.ui -- "
              f"is it a .ui program, and is {binary} the right one (--bin)?")
        return 1
    print(line[line.index("uui_builder:"):])
    if not re.search(r": \d+ windows, \d+ widgets", line):
        return 1                                  # the file's first error, as the builder said it
    # By pid, not by the old title: the last start may have been the
    # builder's error window, which a good file does not bring back.
    while time.time() < deadline:
        new = {p["pid"] for p in dbg.processes_named(prog) if p["name"] == prog} - pids
        if any(w.get("client_pid") in new for w in dbg.windows()):
            return 0
        time.sleep(0.3)
    print(f"ui_push: {binary} read the file but opened no window")
    return 1


if __name__ == "__main__":
    sys.exit(main())
