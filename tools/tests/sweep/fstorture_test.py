#!/usr/bin/env python3
"""Filesystem torture, then power cuts: TFS3, FAT32 and ramfs against a model.

Drives `/tests/fstorture` (userland/tests/fstorture.c), whose every op is
a function of a seed -- so a failure here names a seed that replays it.

PHASE 1, TORTURE (one boot). fsx's shape on TFS3 (`/var`), FAT32 (a
second, virtio disk the host formats with mkfs.fat) and ramfs (`/tmp`):
in-place writes, appends, truncates both ways, holes, renames between
directories and atomic replaces, each checked against an in-memory
model as it goes and the whole tree every 50 ops. Then the guest's
`fsck` must be clean on both disks, and -- after a clean shutdown -- the
HOST reads the final files back with readers SaunaOS did not write the
kernel side of: tools/build/tfs3_writer.py for TFS3, mtools and
`fsck.fat -n` for FAT32. A content hash the guest printed per file must
match what the host reads.

PHASE 2, POWER CUTS (`--cuts N`, a fresh boot each). `fstorture cut`
runs generations of atomic replaces and deletes, each followed by
sync(), and says `durable G` at most once a second. A few seconds in,
QEMU is SIGTERMed (`vm.py stop --hard` -- the guest is never told).
The next boot must then hold, for every file, the version as of the
last generation the log said was durable or a later one -- WHOLE,
never torn, never rolled back (`fstorture check`). `fsck` may find
LEAKED blocks and wrong free counts after a cut, and nothing else:
tfs3's bitmap is written set-before-use, so a crash leaks rather than
double-allocates, by design (vm.py's _acpi_powerdown() says so).
`fsck -r` must then leave TFS3 clean. Cuts alternate TFS3 and FAT32;
every earlier cut's directory is re-checked on each later boot.

WHAT A CUT HERE DOES NOT MODEL: a disk cache that loses writes the
guest had not flushed. QEMU's writes reach the host's page cache, which
survives QEMU's death -- so this proves the guest's own ordering and
its journal, not a drive that lies about a flush.

    python3 tools/tests/sweep/fstorture_test.py               # torture + 4 cuts
    python3 tools/tests/sweep/fstorture_test.py --seed 7 --ops 3000 --cuts 0
    python3 tools/tests/sweep/fstorture_test.py --cuts 1 --positive-control
"""

RUNNER = {"covers": "TFS3/FAT32/ramfs against a model, host-side readers, and power cuts"}

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from gui_debug import DebugConsole  # noqa: E402
import vm as vm_mod  # noqa: E402 -- started_ok()
import port_guard  # noqa: E402
from harness import copy_disk, poll  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
TFS3_WRITER = os.path.join(REPO, "tools/build/tfs3_writer.py")
SECTOR = 512
FAT_START = 2048                 # p1's first sector on the FAT disk
FAT_MIB = 96
FAT_MOUNT = "/mnt/fst"
FAT_MAXKB = 1024                 # eight files must fit the FAT volume with room to spare

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok)))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + ("" if ok or not detail else f"    [{detail}]"))
    return bool(ok)


def report():
    bad = [n for n, ok in checks if not ok]
    print(f"\nfstorture_test: {'FAIL' if bad else 'PASS'} ({len(checks) - len(bad)}/{len(checks)})")
    return 1 if bad else 0


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def fnv(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def make_fat_disk(path, work):
    """A GPT disk with one FAT32 partition, formatted by the host's mkfs.fat."""
    with open(path, "wb") as f:
        f.truncate((FAT_START + FAT_MIB * 2048 + 2048) * SECTOR)
    run(["sfdisk", "--quiet", path], input=f"label: gpt\nstart={FAT_START}, size={FAT_MIB}MiB, "
        "type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=\"fst\"\n", check=True)
    part = os.path.join(work, "fat.part")
    with open(part, "wb") as f:
        f.truncate(FAT_MIB << 20)
    run(["mkfs.fat", "-F", "32", "-s", "8", "-n", "FST", part], check=True)
    run(["dd", f"if={part}", f"of={path}", "bs=512", f"seek={FAT_START}", "conv=notrunc,sparse",
         "status=none"], check=True)
    os.unlink(part)


def fat_part_out(disk, dst):
    """The FAT partition, as its own file, for fsck.fat (which takes no offset)."""
    run(["dd", f"if={disk}", f"of={dst}", "bs=512", f"skip={FAT_START}", f"count={FAT_MIB * 2048}",
         "conv=sparse", "status=none"], check=True)


def root_lba(disk):
    """The TFS3 root's first sector -- the partition tfs3_writer.py recognises."""
    import json
    parts = json.loads(run(["sfdisk", "-J", disk], check=True).stdout)["partitiontable"]["partitions"]
    for p in parts:
        r = run([sys.executable, TFS3_WRITER, "info", "--at-lba", str(p["start"]), disk])
        if r.returncode == 0 and "TFS3" in r.stdout:
            return p["start"]
    return None


class Guest:
    def __init__(self, disk, fatdisk, instance):
        self.disk, self.fatdisk, self.instance = disk, fatdisk, instance
        self.con = None

    def vm(self, *argv):
        cmd = [sys.executable, toolpath.tool("vm.py"), "--instance", str(self.instance), "--disk", self.disk,
               "--extra-disk", self.fatdisk, "--extra-disk-kind", "virtio"]
        return run(cmd + list(argv), cwd=REPO).stdout

    def boot(self):
        if not vm_mod.started_ok(self.vm("start")):
            return False
        self.con = DebugConsole(port_guard.instance_sock(self.instance), timeout=120.0)
        lsblk = self.sh("lsblk")
        disk = next((ln.split()[0] for ln in lsblk.splitlines()
                     if ln.strip().startswith("virtio") and "p" not in ln.split()[0]), None)
        if not disk:
            print(lsblk)
            return False
        self.sh(f"mkdir {FAT_MOUNT}")
        out = self.sh(f"mount {disk}p1 {FAT_MOUNT}")
        return "fat32" in self.sh("mount") and "refused" not in out

    def sh(self, cmd):
        return self.con.send("sh " + cmd) or ""

    def dmesg(self):
        return poll(lambda: self.sh("dmesg"), 10, step=0.5) or ""

    def wait_log(self, pattern, timeout, after=0):
        def ask():
            found = list(re.finditer(pattern, self.dmesg()))
            return found[after] if len(found) > after else None
        return poll(ask, timeout, step=1.0)

    def fsck(self, path, repair=False):
        """The counts fsck prints, as a dict, or None."""
        out = self.sh(f"fsck {'-r ' if repair else ''}{path}")
        got = {}
        for key, label in (("leaked", "leaked (unreferenced)"), ("ref_free", "referenced but free"),
                           ("double", "double-allocated"), ("range", "out-of-range pointers"),
                           ("counts", "free counts wrong"), ("links", "link counts wrong"),
                           ("bad_names", "names to bad records"), ("dirs", "directory errors")):
            m = re.search(re.escape(label) + r":\s+(\d+)", out)
            if m:
                got[key] = int(m.group(1))
        return got if got else None

    def stop(self, hard=False):
        self.vm("stop", *(["--hard"] if hard else []))


def torture(g, args, lba, work):
    print("phase 1: torture")
    seeds = (args.seed, args.seed + 1, args.seed + 2)
    targets = ((f"/var/fst-{seeds[0]}", args.maxkb, "tfs3"),
               (f"{FAT_MOUNT}/fst-{seeds[1]}", FAT_MAXKB, "fat32"),
               (f"/tmp/fst-{seeds[2]}", args.maxkb, "ramfs"))
    for (d, kb, _), s in zip(targets, seeds):
        g.sh(f"spawn /tests/fstorture run {d} {s} {args.ops} {kb}")
    files = {}
    for (d, kb, fs), s in zip(targets, seeds):
        m = g.wait_log(rf"fstorture: (PASSED run seed {s},|FAIL run seed {s} )", 900)
        log = g.dmesg()
        why = next((ln.strip() for ln in log.splitlines() if f"FAIL run seed {s} " in ln), "no result")
        if not check(f"{fs}: {args.ops} ops against the model (seed {s})",
                     m is not None and m.group(1).startswith("PASSED"), why):
            continue
        files[fs] = [(p, int(n), int(h, 16)) for p, n, h in
                     re.findall(rf"fstorture: file ({re.escape(d)}/\S+) (\d+) ([0-9a-f]{{16}})", log)]

    for path, fs in (("/", "tfs3"), (FAT_MOUNT, "fat32")):
        r = g.fsck(path)
        check(f"{fs}: the guest's fsck is clean afterwards", r is not None and not any(r.values()), r)
    g.sh("sync")   # the host reads the image next: nothing may still be in RAM
    g.stop()

    # The host reads the same files with readers of its own.
    if files.get("tfs3"):
        bad = []
        for p, n, h in files["tfs3"]:
            out = os.path.join(work, "read.bin")
            r = run([sys.executable, TFS3_WRITER, "read", "--at-lba", str(lba), g.disk, p, out])
            data = open(out, "rb").read() if r.returncode == 0 else None
            if data is None or len(data) != n or fnv(data) != h:
                bad.append(p)
        check(f"tfs3: the host's own reader gets every file's bytes ({len(files['tfs3'])} files)",
              not bad, f"differ: {bad}")
    if files.get("fat32"):
        part = os.path.join(work, "fat.part")
        fat_part_out(g.fatdisk, part)
        r = run(["fsck.fat", "-n", part])
        check("fat32: the host's fsck.fat finds nothing", r.returncode == 0, (r.stdout + r.stderr)[-400:])
        bad = []
        for p, n, h in files["fat32"]:
            out = os.path.join(work, "read.bin")
            rel = p[len(FAT_MOUNT):]
            r = run(["mcopy", "-n", "-o", "-i", part, "::" + rel, out])
            data = open(out, "rb").read() if r.returncode == 0 else None
            if data is None or len(data) != n or fnv(data) != h:
                bad.append(p)
        check(f"fat32: mtools reads every file's bytes ({len(files['fat32'])} files)", not bad, f"differ: {bad}")
        os.unlink(part)


ALLOWED_AFTER_CUT = ("leaked", "counts")


def after_cut(g, cuts, label):
    """Every earlier cut's directory is whole, and fsck finds only leaks."""
    for path, fs in (("/", "tfs3"), (FAT_MOUNT, "fat32")):
        r = g.fsck(path)
        if not check(f"{label}: {fs} fsck after the cut reads", r is not None):
            continue
        damage = {k: v for k, v in r.items() if v and k not in ALLOWED_AFTER_CUT}
        check(f"{label}: {fs} has nothing worse than leaked blocks "
              f"(leaked {r.get('leaked', 0)}, free counts {r.get('counts', 0)})", not damage, damage)
        if fs == "tfs3" and any(r.values()):
            g.fsck(path, repair=True)
            r2 = g.fsck(path)
            check(f"{label}: tfs3 is clean after fsck -r", r2 is not None and not any(r2.values()), r2)
    for d, s, gen in cuts:
        seen = len(re.findall(rf"fstorture: (PASSED|FAIL) check {re.escape(d)} ", g.dmesg()))
        g.sh(f"spawn /tests/fstorture check {d} {s} {gen}")
        m = g.wait_log(rf"fstorture: (PASSED check {re.escape(d)} [^\n]*|FAIL check seed {s} [^\n]*)",
                       300, after=seen)
        check(f"{label}: {d} holds generation {gen} or later, whole",
              m is not None and m.group(1).startswith("PASSED"), m.group(1) if m else "no result")


def power_cuts(g, args):
    print(f"phase 2: {args.cuts} power cuts")
    rng = random.Random(args.seed)
    cuts = []   # (dir, seed, durable generation)
    for i in range(1, args.cuts + 1):
        label = f"cut {i}"
        if not check(f"{label}: the guest boots after the last one", g.boot()):
            return
        if cuts:
            replay = "replayed a committed journal transaction" in g.dmesg()
            print(f"    (journal replay at this boot: {'yes' if replay else 'no'})")
            after_cut(g, cuts, label)
        seed = args.seed * 100 + i
        d = f"/var/cut-{seed}" if i % 2 else f"{FAT_MOUNT}/cut-{seed}"
        g.sh(f"spawn /tests/fstorture cut {d} {seed}")
        first = g.wait_log(r"fstorture: durable (\d+)", 120)
        if not check(f"{label}: the cut run on {d} makes a generation durable", first is not None):
            g.stop(hard=True)
            return
        wait = rng.uniform(2.0, 10.0)
        time.sleep(wait)  # sleep-ok: the cut lands at a random point -- the delay IS the test
        log = g.dmesg()
        gens = [int(x) for x in re.findall(r"fstorture: durable (\d+)", log.split(f"cut {d} seed")[-1])]
        fail = re.search(r"fstorture: FAIL cut [^\n]*", log)
        check(f"{label}: no cut-mode failure before the power goes", not fail, fail.group(0) if fail else "")
        g.stop(hard=True)
        cuts.append((d, seed, max(gens)))
        print(f"    power cut after {wait:.1f} s, durable through generation {max(gens)}")
    if check("final boot after the last cut", g.boot()):
        after_cut(g, cuts, "final")
        if args.positive_control and cuts:
            control(g, cuts[0])
        g.stop()


def control(g, cut):
    """The checker must be able to fail: a torn file, and a rollback."""
    d, seed, gen = cut
    print("positive control: the check must go red")
    for claimed, label, want in ((gen + 1_000_000, "a durable generation that never ran", "ROLLED BACK|which no"),
                                 (gen, "one byte flipped in a file", "torn")):
        if want == "torn":
            names = [ln.strip() for ln in g.sh(f"ls {d}").splitlines() if re.fullmatch(r"c\d+", ln.strip())]
            if not check("control: the cut left a file to corrupt", names, f"ls {d} found none"):
                return
            victim = f"{d}/{names[0]}"
            g.sh(f"spawn /tests/fstorture poke {victim} 100")
            g.wait_log(rf"fstorture: poked {re.escape(victim)}", 30)
        seen = len(re.findall(rf"FAIL check seed {seed} ", g.dmesg()))
        g.sh(f"spawn /tests/fstorture check {d} {seed} {claimed}")
        m = g.wait_log(rf"fstorture: FAIL check seed {seed} [^\n]*", 300, after=seen)
        check(f"control: {label} is caught", m is not None and re.search(want, m.group(0)),
              m.group(0) if m else "the check passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=None, help="default: random, printed")
    ap.add_argument("--ops", type=int, default=1500)
    ap.add_argument("--maxkb", type=int, default=2560, help="largest file, KiB (2560 reaches TFS3's double-indirect)")
    ap.add_argument("--cuts", type=int, default=4)
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--positive-control", action="store_true",
                    help="after the cuts, show the check failing on a torn file and a rollback")
    args = ap.parse_args()
    if args.seed is None:
        args.seed = random.randrange(1, 1 << 20)
    print(f"fstorture_test: seed {args.seed}")
    if args.instance == "auto":
        args.instance = port_guard.find_free_instance()
        if args.instance is None:
            print("fstorture_test: no free VM slot")
            return 2
    args.instance = int(args.instance)
    for tool in ("sfdisk", "mkfs.fat", "fsck.fat", "mcopy", "dd"):
        if not shutil.which(tool):
            print(f"fstorture_test: needs {tool} on the host")
            return 2

    work = tempfile.mkdtemp(prefix="fstorture-")
    try:
        disk, fatdisk = os.path.join(work, "disk.img"), os.path.join(work, "fat.img")
        copy_disk(os.path.join(REPO, "disk.img"), disk)
        make_fat_disk(fatdisk, work)
        lba = root_lba(disk)
        if not check("the host finds the TFS3 root on the disk copy", lba is not None):
            return report()
        g = Guest(disk, fatdisk, args.instance)
        if not check("the guest boots with the FAT32 disk mounted", g.boot()):
            g.stop()
            return report()
        torture(g, args, lba, work)
        if args.cuts:
            power_cuts(g, args)
    finally:
        try:
            Guest(os.path.join(work, "disk.img"), os.path.join(work, "fat.img"), args.instance).stop(hard=True)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    return report()


if __name__ == "__main__":
    sys.exit(main())
