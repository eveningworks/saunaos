#!/usr/bin/env python3
"""/bin/update cut off by power at a random moment: every file is old or new.

WHAT IT PROVES. A machine whose update is interrupted -- QEMU killed at a
random instant of a run, and once during the boot that applies what the
run staged -- must come back up, hold every managed file as its OLD bytes
or the manifest's NEW ones (never torn, never missing), and finish the job
on the next `update`. The design promises exactly that much: "not an
atomic update", but verify-everything-then-commit, each file renamed over
its target, and a boot-time apply that a power cut leaves for the boot
after (docs/update-design.md).

HOW. A damaged image is made once: files the harness never runs (documents,
a demo binary, /bin/hello, /lib/libhello.so -- staged for the boot) are
truncated, two documents deleted. One uncut run measures how long an update
takes. Each cut then copies the damaged image, starts `/bin/update` in the
background, kills the guest a random time into that span, boots again, and
reads every target back with the guest's own `sum`. The OLD version of a
truncated file is the first 100 bytes of the shipped one, so both sides of
the check come from the host's staging tree, not from update's own report.

**IT NEVER LEAVES THIS MACHINE**: update_test.py's server, in this process
on a loopback port, reached through SLIRP's 10.0.2.2.

    python3 tools/tests/sweep/update_cut_test.py [--cuts N] [--seed S]
    python3 tools/tests/sweep/update_cut_test.py --positive-control

THE POSITIVE CONTROL tears one target after the first cut (50 bytes of it,
by the guest's own truncate, before the read-back), so a check that cannot
see a torn file shows up red.
"""

RUNNER = {"covers": "/bin/update interrupted by power: every file old or new, finished next run"}

import argparse
import os
import random
import shutil
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
ROOT = os.path.dirname(HERE)

from net_test import launch, kill  # noqa: E402
import update_server  # noqa: E402
from harness import Results, copy_disk  # noqa: E402
from update_test import Server, summed, GATEWAY  # noqa: E402

STAGING = os.path.join(ROOT, "seed", "sync")
CUT = 100   # what a damaged file keeps
# Never run by the harness itself (it reads back through sum, cat, ls).
TRUNCATED = [
    "/bin/hello",
    "/lib/libhello.so",                      # staged for the boot: a library
    "/usr/share/doc/guide/kernel.md",
    "/usr/share/doc/guide/storage.md",
    "/usr/share/doc/guide/shell.md",
    "/usr/share/doc/guide/network.md",
    "/usr/share/doc/cmd/diskbench.md",
    "/usr/share/doc/cmd/update.md",
    "/usr/share/doc/cmd/fsck.md",
    "/usr/share/doc/cmd/ls.md",
]
DELETED = [
    "/usr/share/doc/cmd/cat.md",
    "/usr/share/doc/cmd/sum.md",
]


def present(paths):
    """The targets this build ships -- a doc renamed since keeps nothing red."""
    return [p for p in paths if os.path.isfile(os.path.join(STAGING, p.lstrip("/")))]


def shipped(path):
    with open(os.path.join(STAGING, path.lstrip("/")), "rb") as f:
        data = f.read()
    return zlib.crc32(data) & 0xFFFFFFFF, len(data), data


def allowed(path):
    """(crc, size) pairs a file may be after a cut: old, new -- None is absent."""
    crc, size, data = shipped(path)
    new = (crc, size)
    if path in DELETED:
        return {None, new}
    old = data[:CUT]
    return {(zlib.crc32(old) & 0xFFFFFFFF, len(old)), new}


def boot(disk, tmp, tag):
    sh, pidfile = launch(disk, tmp, tag, "e1000")
    return sh, pidfile


def check_targets(c, sh, targets, label):
    bad = []
    for p in targets:
        got = summed(sh, p)
        if got not in allowed(p):
            bad.append(f"{p} = {got}")
    c.ok(f"{label}: every target is its old or its new version", not bad, "; ".join(bad)[:300])
    return not bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--cuts", type=int, default=6, help="interrupted runs (default 6)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--positive-control", action="store_true",
                    help="tear one target after the first cut; the check must go red")
    args = ap.parse_args()

    disk_src = os.path.join(ROOT, "disk.img")
    if not os.path.exists(disk_src):
        print("update_cut_test: SKIP -- no disk.img; run `make iso` first")
        return 0
    why = update_server._stale_reason(STAGING)
    if why:
        print(f"update_cut_test: FAIL -- the staging tree is stale ({why}); run `make iso`")
        return 1
    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    rng = random.Random(seed)
    print(f"update_cut_test: seed {seed}")

    truncated, deleted = present(TRUNCATED), present(DELETED)
    DELETED[:] = deleted
    targets = truncated + deleted
    c = Results()
    tmp = tempfile.mkdtemp(prefix="update_cut.")
    srv = Server()
    url = f"http://{GATEWAY}:{srv.port}"
    pidfile = None
    try:
        # The damaged image, once.
        damaged = os.path.join(tmp, "damaged.img")
        copy_disk(disk_src, damaged)
        sh, pidfile = boot(damaged, tmp, "prep")
        for p in truncated:
            sh.run(f"truncate -s {CUT} {p}")
        for p in deleted:
            sh.run(f"rm {p}")
        sh.run("sync")
        kill(pidfile)
        pidfile = None

        # One run uncut: how long the span to cut in is, and that it ends right.
        work = os.path.join(tmp, "work.img")
        copy_disk(damaged, work)
        sh, pidfile = boot(work, tmp, "whole")
        t0 = time.time()
        out = sh.run(f"update --from {url}", timeout=900)
        span = time.time() - t0
        c.ok("an uncut run installs and stages", "staged" in out or "updated" in out, out.strip()[-200:])
        sh.run("sync")
        kill(pidfile)
        pidfile = None
        print(f"update_cut_test: an uncut run takes {span:.1f} s")

        for i in range(args.cuts):
            copy_disk(damaged, work)
            sh, pidfile = boot(work, tmp, f"cut{i}")
            at = rng.uniform(0.05, span * 1.1)
            sh.run(f"spawn /bin/update --from {url}")
            time.sleep(at)
            kill(pidfile)
            pidfile = None
            time.sleep(0.5)
            sh, pidfile = boot(work, tmp, f"after{i}")
            if args.positive_control and i == 0:
                sh.run(f"truncate -s 50 {truncated[0]}")
            label = f"cut {i} at {at:.2f} s"
            c.ok(f"{label}: the machine boots", sh is not None)
            check_targets(c, sh, targets, label)
            out = sh.run(f"update --from {url}", timeout=900)
            if "restart" in out:
                sh.run("sync")
                kill(pidfile)
                pidfile = None
                sh, pidfile = boot(work, tmp, f"fin{i}")
            new = [p for p in targets if summed(sh, p) != shipped(p)[:2]]
            c.ok(f"{label}: the next update finishes the job", not new, ", ".join(new)[:300])
            kill(pidfile)
            pidfile = None

        # A cut DURING the boot that applies the staged library.
        copy_disk(damaged, work)
        sh, pidfile = boot(work, tmp, "stage")
        out = sh.run(f"update --from {url}", timeout=900)
        staged = "/lib/libhello.so" in sh.run("cat /var/lib/update/pending")
        c.ok("a run stages the library for the boot", staged, out.strip()[-160:])
        sh.run("sync")
        kill(pidfile)
        pidfile = None
        if staged:
            # Launch without waiting: kill early in the boot, where the apply runs.
            boot_cmd_pid = os.path.join(tmp, "early.pid")
            net_test_launch_nowait(work, tmp, boot_cmd_pid)
            time.sleep(rng.uniform(0.3, 2.5))
            kill(boot_cmd_pid)
            time.sleep(0.5)
            sh, pidfile = boot(work, tmp, "afterapply")
            check_targets(c, sh, ["/lib/libhello.so"], "a cut during the boot-time apply")
            sh.run("sync")
            kill(pidfile)
            pidfile = None
            sh, pidfile = boot(work, tmp, "afterapply2")
            c.ok("...and the boot after it finishes the apply",
                 summed(sh, "/lib/libhello.so") == shipped("/lib/libhello.so")[:2])
            c.ok("...leaving no pending list",
                 "no such file" in sh.run("cat /var/lib/update/pending"))
    finally:
        if pidfile:
            kill(pidfile)
        srv.srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    bad = c.failed()
    print(f"\nupdate_cut_test: {len(c.rows) - len(bad)}/{len(c.rows)} checks passed (seed {seed})")
    return 1 if bad else 0


def net_test_launch_nowait(disk, tmp, pidfile):
    """The same QEMU line as net_test.launch(), without waiting for a shell."""
    import subprocess
    import net_test
    sock = os.path.abspath(os.path.join(tmp, "early.serial"))
    boot = " ".join(net_test.guarded_boot_args(disk, os.path.join(ROOT, "saunaos.iso")))
    cmd = (f"qemu-system-x86_64 {boot}"
           f" -drive file={disk},format=raw,if=ide,discard=unmap"
           f"{net_test.nic_args('e1000', None, '', None)}"
           f" -m 1024 -display none -no-reboot"
           f" -serial unix:{sock},server,nowait"
           f" -daemonize -pidfile {pidfile}")
    subprocess.run(cmd, shell=True, check=True, cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
