#!/usr/bin/env python3
"""`w16 --install` and its dialog, in a guest, from disks with no Microsoft file.

lib/uwinsetup is checked on the host (tools/uwinsetup_hostcheck.py); this
is the rest of the path, in a real toy-os: w16 reading the disks inside
its sandbox, writing only drive C:, and the Install Windows 3.x dialog
driven from Retro Launcher's bar to the folder it adds.

The disks are tools/gen_win3_disks.py's: four 1.44 MB floppy images, and
the same set unpacked into a folder, each file only its own name. That
generator's table says where each file must land.

What it asserts:

  - the CLI: --dry-run plans every file and changes nothing; the install
    puts every one where the table says (sizes and two contents read
    back), the decoys for other hardware absent, exit status 0
  - an existing WIN.INI kept, then made afresh with --replace-ini
  - a set missing one file: exit status 1, the file named, nothing
    written for it
  - the dialog: Retro Launcher shows its bar while drive C: has no
    Windows; the bar opens the dialog; the Disks path and Next reach
    Check, Install reaches Done; Retro Launcher's folders gain
    C:\\WINDOWS as "Windows" and its bar goes

    python3 tools/w16_install_test.py [--instance N] [--kvm]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_win3_disks  # noqa: E402
import port_guard  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print("  %s  %s%s" % ("PASS" if ok else "FAIL", name, ("   [%s]" % detail[-200:]) if detail and not ok else ""))
    return ok


class Guest:
    def __init__(self, instance, kvm, disk):
        self.base = [sys.executable, os.path.join(ROOT, "tools", "vm.py"), "--instance", str(instance)]
        self.kvm, self.disk = kvm, disk

    def vm(self, *args, timeout=300):
        r = subprocess.run(self.base + list(args), capture_output=True, text=True, timeout=timeout, cwd=ROOT)
        return r.stdout + r.stderr

    def start(self):
        out = self.vm(*(["--kvm"] if self.kvm else []), "--disk", self.disk, "start", timeout=400)
        return "vm: ready" in out, out

    def sh(self, *cmds):
        return self.vm("exec", *cmds)

    def put(self, src, dst):
        return self.vm("put", src, dst)

    def stop(self):
        self.vm("stop")


def widget(g, window, name):
    """A named widget's screen centre in `window`, or None when it is not shown."""
    for line in g.sh("guictl widgets '%s'" % window).splitlines():
        p = line.split()
        if len(p) >= 7 and p[0] == name:
            w, h = map(int, p[3].split("x"))
            return int(p[5]) + w // 2, int(p[6]) + h // 2
    return None


def wait_log(g, pattern, want=None, timeout=40.0):
    """The last match of `pattern` in the log, once it is `want` (any, when None) -- the
    dialog logs its first step at once, so a poll must wait for the one it expects."""
    last = None
    end = time.time() + timeout
    while time.time() < end:
        m = re.findall(pattern, g.sh("log -n 400"))
        if m:
            last = m[-1]
            if want is None or last == want:
                return last
        time.sleep(1.0)
    return last


def cli(g, tmp):
    print("w16 --install:")
    out = g.sh("w16 --install /tmp/disks --dry-run", "ls /home/Win16")
    want = gen_win3_disks.build(os.path.join(tmp, "unused"), "flat")
    check("--dry-run reads the four images and plans every file",
          "disks 4" in out and "files %d" % len(want) in out and "missing 0" in out, out)
    check("...and changes nothing", "WINDOWS" not in out.split("--- ls /home/Win16 ---")[-1], out)
    # The console names a non-zero exit ("w16: exit 1"); silence is 0.
    out = g.sh("w16 --install /tmp/disks")
    check("the install runs to its end, exit status 0", "done %d files" % len(want) in out and "w16: exit" not in out,
          out)
    listing = g.sh("ls -l /home/Win16/WINDOWS", "ls -l /home/Win16/WINDOWS/SYSTEM")
    sizes = {}
    where = None
    for line in listing.splitlines():
        if line.startswith("--- ls -l "):
            where = "WINDOWS/SYSTEM" if line.rstrip(" -").endswith("SYSTEM") else "WINDOWS"
            continue
        p = line.split()
        if len(p) >= 5 and p[0] == "-":
            sizes["%s/%s" % (where, p[-1])] = int(p[1])
    bad = sorted(set(want) ^ set(sizes)) + sorted(p for p in want if p in sizes and sizes[p] != len(want[p]))
    check("every file in its folder at its size, nothing else (%d files)" % len(want), not bad, ", ".join(bad[:6]))
    out = g.sh("cat /home/Win16/WINDOWS/SYSTEM/MAIN.CPL", "cat /home/Win16/WINDOWS/WINVER.EXE")
    check("their contents expand from the KWAJ files", "test file MAIN.CPL" in out and "MZw16 installer test file WINVER" in out,
          out[:200])

    g.sh("rm /home/Win16/WINDOWS/WIN.INI")
    g.put(os.path.join(tmp, "mine.ini"), "/home/Win16/WINDOWS/WIN.INI")
    out = g.sh("w16 --install /tmp/disks", "cat /home/Win16/WINDOWS/WIN.INI")
    check("an existing WIN.INI is kept", "[mine]" in out.split("--- cat")[-1], out[-200:])
    out = g.sh("w16 --install /tmp/disks --replace-ini", "cat /home/Win16/WINDOWS/WIN.INI")
    check("...and made afresh with --replace-ini", "test file WIN.SRC" in out.split("--- cat")[-1], out[-200:])

    out = g.sh("rm /tmp/flat/CALC.EX_", "rm /home/Win16/WINDOWS/CALC.EXE", "w16 --install /tmp/flat",
               "ls /home/Win16/WINDOWS")
    check("a set missing CALC.EXE: exit status 1, the file named",
          "w16: exit 1" in out and "missing C:\\WINDOWS\\CALC.EXE" in out, out[-300:])
    check("...and nothing written for it", "CALC.EXE" not in out.split("--- ls /home/Win16/WINDOWS ---")[-1],
          out[-200:])


def dialog(g):
    print("the dialog:")
    g.sh("rm -r /home/Win16/WINDOWS")
    g.sh("guictl open 'Retro Launcher'")
    time.sleep(4)
    bar = widget(g, "Retro Launcher", "winbar")
    check("Retro Launcher shows its bar while drive C: has no Windows", bar is not None)
    if not bar:
        return
    # The bar's button sits at its right end: a click there is the bar's action.
    x, y = bar
    for line in g.sh("guictl widgets 'Retro Launcher'").splitlines():
        p = line.split()
        if p and p[0] == "winbar":
            w = int(p[3].split("x")[0])
            x = int(p[5]) + w - 60
    g.sh("guictl click %d %d" % (x, y))
    time.sleep(4)
    src = widget(g, "Install Windows 3.x", "src")
    check("the bar opens Install Windows 3.x", src is not None, g.sh("guictl windows"))
    if not src:
        return
    g.sh("guictl click %d %d" % src)
    g.sh(*["guictl key %s" % c for c in "/tmp/disks"])
    g.sh("guictl key 0x0A")
    time.sleep(1)
    # Next: the wizard's own button, last in its bar.
    nxt = widget(g, "Install Windows 3.x", "next") or widget(g, "Install Windows 3.x", "wizard_next")
    if nxt:
        g.sh("guictl click %d %d" % nxt)
    step = wait_log(g, r"wininstall: step (\d)", "1", 60)
    check("Next checks the disks and reaches Check", step == "1", "step %s" % step)
    if nxt:
        g.sh("guictl click %d %d" % nxt)
    done = wait_log(g, r"wininstall: (done|FAILED|STOPPED)", None, 90)
    check("Install runs to Done", done == "done", str(done))
    folders = g.sh("cat /etc/win16.conf")
    check("Retro Launcher's folders gain C:\\WINDOWS as Windows",
          re.search(r"folder(\d+)=/home/Win16/WINDOWS\n(?:.*\n)*?folder\1_name=Windows", folders) is not None,
          folders[-300:])
    if nxt:
        g.sh("guictl click %d %d" % nxt)        # Close: the launcher has the focus back, and looks again
    time.sleep(3)
    check("...and its bar goes", widget(g, "Retro Launcher", "winbar") is None,
          g.sh("guictl windows"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=None)
    ap.add_argument("--kvm", action="store_true")
    args = ap.parse_args()
    instance = args.instance if args.instance is not None else port_guard.find_free_instance()
    tmp = tempfile.mkdtemp(prefix="w16-install-")
    try:
        disk = os.path.join(tmp, "disk.img")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "fresh_disk.py"), "--make", disk],
                           capture_output=True, text=True, cwd=ROOT)
        if r.returncode != 0:
            print("w16_install_test: no fresh disk: %s" % r.stderr.strip())
            return 1
        gen_win3_disks.build(os.path.join(tmp, "images"), "images")
        gen_win3_disks.build(os.path.join(tmp, "flat"), "flat")
        with open(os.path.join(tmp, "mine.ini"), "wb") as f:
            f.write(b"[mine]\r\n")
        g = Guest(instance, args.kvm, disk)
        ok, out = g.start()
        if not check("the guest boots", ok, out):
            return 1
        try:
            g.sh("mkdir /tmp/disks", "mkdir /tmp/flat")
            for d, sub in ((os.path.join(tmp, "images"), "/tmp/disks"), (os.path.join(tmp, "flat"), "/tmp/flat")):
                for name in sorted(os.listdir(d)):
                    g.put(os.path.join(d, name), "%s/%s" % (sub, name))
            cli(g, tmp)
            dialog(g)
        finally:
            g.stop()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    failed = [n for n, ok in results if not ok]
    print("\nw16_install_test: %d passed, %d failed" % (len(results) - len(failed), len(failed)))
    for n in failed:
        print("  FAILED:", n)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
