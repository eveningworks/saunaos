#!/usr/bin/env python3
"""The utf8migrate SERVICE across two boots, judged on the host.

`/tests/utf8migrate_test` (usertest) checks the program over a fixture
with `--root`; this checks the part it cannot: that the boot runs it on
its own, over the real root, ONCE. A copy of disk.img gets Latin-1 names
and contents written from the host (tfs3_writer, raw bytes -- the serial
console would mangle them), boots, and is read back on the host after a
clean shutdown: the names and bytes converted, an already-UTF-8 name
untouched, the marker and the log written. Then a second boot must log
"nothing to do" and change nothing.

THE ORACLE IS THE HOST'S READER, never the guest's console: what is on
the disk is the claim.

--positive-control deletes the service's descriptor from the copy, so
nothing runs at boot and the conversion checks must go red.

Usage: python3 tools/utf8migrate_boot_test.py [--instance N] [--positive-control]
Exits 0 when every check passed, 1 otherwise, 2 if it could not run.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import tfs3_writer as t3        # noqa: E402
import mkpart_test              # noqa: E402
import vm as vm_mod             # noqa: E402
from harness import copy_disk   # noqa: E402
import iso_guard                # noqa: E402 -- refuses stale media, holds the tree lock

VM = os.path.join(REPO, "tools", "vm.py")
results = []


def check(what, ok, detail=""):
    results.append(bool(ok))
    print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f"   -- {detail}" if detail and not ok else ""))


def d(b):
    """Raw disk bytes in tfs3_writer's byte-per-char form."""
    return b.decode("latin-1")


class Img:
    def __init__(self, disk, writable=False):
        base, sectors = mkpart_test.volume_of(disk)
        self.img = t3.Tfs3Image(disk, writable=writable, base_lba=base, sectors=sectors)

    def read(self, path):
        ino = self.img.lookup(d(path))
        return None if ino is None else bytes(self.img.read_file_data(ino))

    def names(self, path):
        ino = self.img.lookup(d(path))
        return set() if ino is None else {n.encode("latin-1") for n, _ in self.img.dirents(ino)} - {b".", b".."}

    def close(self):
        self.img.close()


def vm(disk, instance, *argv, timeout=300):
    r = subprocess.run([sys.executable, VM, "--disk", disk, "--instance", str(instance), *argv],
                       cwd=REPO, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def boot(disk, instance):
    """Boot, wait for the service to have finished, return dmesg's
    utf8migrate lines, shut down cleanly."""
    rc, out = vm(disk, instance, "start")
    if not vm_mod.started_ok(out):
        print("utf8migrate_boot_test: the VM did not start\n" + out[-2000:])
        sys.exit(2)
    lines = []
    try:
        deadline = time.time() + 120
        while time.time() < deadline:
            rc, out = vm(disk, instance, "exec", "dmesg")
            lines = [ln for ln in out.splitlines() if "utf8migrate" in ln]
            if any("exited with code" in ln or "nothing to do" in ln for ln in lines):
                break
            time.sleep(1)
        vm(disk, instance, "exec", "sync")
    finally:
        vm(disk, instance, "stop")
    return lines


FIXTURE = {
    b"/home/u8boot/K\xe4se.txt": b"\xe4\xf6\n",              # name and contents Latin-1
    b"/home/u8boot/Caf\xc3\xa9.txt": b"caf\xc3\xa9\n",       # already UTF-8: untouched
    b"/etc/u8boot.conf": b"na\xefve\n",                      # /etc contents
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    src = os.path.join(REPO, "disk.img")
    if not os.path.exists(src):
        print("utf8migrate_boot_test: no disk.img -- run `make iso` first")
        return 2
    iso_guard.assert_iso_fresh(medium="disk")

    tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
    tmp.close()
    disk = tmp.name
    try:
        copy_disk(src, disk)
        img = Img(disk, writable=True)
        for gone in (b"/var/lib/utf8migrate", b"/var/log/utf8migrate.log") + \
                    ((b"/etc/services.d/utf8migrate",) if args.positive_control else ()):
            if img.img.lookup(d(gone)) is not None:
                t3.delete_path(img.img, d(gone))
        t3.mkdir_path(img.img, d(b"/home/u8boot"))
        for path, data in FIXTURE.items():
            t3.write_file(img.img, data, d(path))
        img.close()

        print("boot 1")
        lines = boot(disk, args.instance)
        img = Img(disk)
        check("the boot ran the conversion",
              any("utf8migrate: 1 renamed, 2 converted" in ln for ln in lines), lines)
        check("the Latin-1 name is UTF-8 now",
              img.names(b"/home/u8boot") == {b"K\xc3\xa4se.txt", b"Caf\xc3\xa9.txt"},
              img.names(b"/home/u8boot"))
        check("...and its contents", img.read(b"/home/u8boot/K\xc3\xa4se.txt") == b"\xc3\xa4\xc3\xb6\n",
              img.read(b"/home/u8boot/K\xc3\xa4se.txt"))
        check("an already-UTF-8 file is untouched",
              img.read(b"/home/u8boot/Caf\xc3\xa9.txt") == b"caf\xc3\xa9\n")
        check("an /etc file's contents converted", img.read(b"/etc/u8boot.conf") == b"na\xc3\xafve\n",
              img.read(b"/etc/u8boot.conf"))
        marker = img.read(b"/var/lib/utf8migrate") or b""
        check("the marker is written", marker.startswith(b"converted "), marker)
        log = img.read(b"/var/log/utf8migrate.log") or b""
        check("the log names the old bytes and the new name",
              b"rename  /home/u8boot/K\\xE4se.txt -> /home/u8boot/K\xc3\xa4se.txt" in log, log[:400])
        snap = {p: img.read(p) for p in (b"/home/u8boot/K\xc3\xa4se.txt", b"/etc/u8boot.conf",
                                         b"/var/lib/utf8migrate", b"/var/log/utf8migrate.log")}
        img.close()

        print("boot 2")
        lines = boot(disk, args.instance)
        img = Img(disk)
        check("the second boot does nothing, and says so",
              any("utf8migrate: nothing to do -- converted" in ln for ln in lines), lines)
        after = {p: img.read(p) for p in snap}
        check("...and the disk is as the first boot left it", after == snap)
        img.close()
    finally:
        os.unlink(disk)

    bad = results.count(False)
    print(f"utf8migrate_boot_test: {len(results) - bad}/{len(results)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
