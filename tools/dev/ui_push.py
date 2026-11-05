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
import port_guard  # noqa: E402

REPO = toolpath.REPO
GUEST_DIR = "/usr/share/ui"   # UUI_BUILDER_DIR, userland/libs/ui/uui_builder.h


def sources(name):
    """Every tracked <name>.ui beside a program's source -- a component is
    two or three folders under userland/apps/, as the Makefile's
    COMPONENT_MKS finds them."""
    return sorted(glob.glob(os.path.join(REPO, "userland/apps/*/*/" + name + ".ui")) +
                  glob.glob(os.path.join(REPO, "userland/apps/*/*/*/" + name + ".ui")))


def find_ui(arg):
    """(the .ui file, the program's name). A name is looked up beside the
    program's source, where the build seeds it from."""
    if arg.endswith(".ui"):
        if not os.path.isfile(arg):
            sys.exit(f"ui_push: no file {arg}")
        return arg, os.path.basename(arg)[:-3]
    hits = sources(arg)
    if not hits:
        sys.exit(f"ui_push: no {arg}.ui under userland/apps/ -- pass the file's path")
    return hits[0], arg


def guest_binary(name):
    """The Makefile's seed mapping: userland/apps/<class>/<dir>/ is
    /bin/wm/<class>/<dir>, <class> being every folder above <dir> (so
    apps/a/b/c/ is /bin/wm/a/b/c) and <dir> renamed by the manifest's
    INSTALL_AS. Found from the file's name, so an edited copy kept
    elsewhere still finds its program."""
    hits = sources(name)
    if not hits:
        sys.exit(f"ui_push: no program has a {name}.ui in the tree -- say which with --bin")
    d = os.path.dirname(hits[0])
    rel = os.path.relpath(d, os.path.join(REPO, "userland/apps"))
    prog = os.path.basename(rel)
    mk = os.path.join(d, "component.mk")
    if os.path.isfile(mk):
        m = re.search(r"^INSTALL_AS\s*=\s*(\S+)", open(mk).read(), re.M)
        if m:
            prog = m.group(1)
    return f"/bin/wm/{os.path.dirname(rel)}/{prog}"


def program_windows(dbg, pids):
    return [w for w in dbg.windows() if w.get("client_pid") in pids]


def close_all(dbg, pids, timeout):
    """Ask every window of `pids` to close, as its close button does, and
    wait until NONE is left -- by pid, not title: two Calculators share
    one, and another program's window may too. False when one stays (an
    app may refuse, or ask first)."""
    # Highest z first: a close does not renumber the windows below it.
    for w in sorted(program_windows(dbg, pids), key=lambda w: -w["z"]):
        dbg.send(f"gui close {w['z']}")
    deadline = time.time() + timeout
    while program_windows(dbg, pids):
        if time.time() > deadline:
            return False
        time.sleep(0.2)
    return True


def builder_line(dbg, name):
    """The newest `uui_builder:` line about <name>.ui, or None."""
    hits = [l.strip() for l in dbg.logs("uui_builder:", clear=False) if f"{name}.ui" in l]
    return hits[-1][hits[-1].index("uui_builder:"):] if hits else None


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

    dbg = DebugConsole(os.path.join(REPO, port_guard.instance_sock(args.instance)))
    binary = args.bin or guest_binary(name)
    prog = os.path.basename(binary)
    pids = {p["pid"] for p in dbg.processes_named(prog) if p["name"] == prog}
    if not close_all(dbg, pids, args.timeout):
        print(f"ui_push: a window of {prog} did not close (did it ask something?) -- not restarted")
        return 1

    dbg.logs("uui_builder:")                      # drop older starts' lines
    m = re.search(r"as pid (\d+)", dbg.send(f"gui spawn {binary}") or "")
    if not m:
        print(f"ui_push: {binary} did not start -- is it the right program (--bin)?")
        return 1
    pid = int(m.group(1))
    # THE VERDICT IS THE WINDOW, not the builder's load line: a file that
    # parses is logged as built before the program's lookups run, and a
    # name or type it lacks opens the builder's error window instead
    # (uui_builder_run_error(), app id `uierror`, which logs the error).
    deadline = time.time() + args.timeout
    while True:
        wins = [w for w in program_windows(dbg, {pid}) if w.get("first_frame", {"w": 1})["w"] > 0]
        if wins:
            break
        if time.time() > deadline or not any(p["pid"] == pid for p in dbg.processes()):
            print(f"ui_push: {binary} opened no window -- {builder_line(dbg, name) or 'it never read ' + name + '.ui'}")
            return 1
        time.sleep(0.3)
    line = builder_line(dbg, name)
    print(line or f"ui_push: {binary} opened a window without reading {name}.ui -- the right program (--bin)?")
    return 1 if wins[0].get("app_id") == "uierror" or line is None else 0


if __name__ == "__main__":
    sys.exit(main())
