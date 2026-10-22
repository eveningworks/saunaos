#!/usr/bin/env python3
"""toy-os's exFAT, judged by the host: fsck.exfat and an independent reader.

kernel/fs/exfat*.c reads and writes a format another OS has to accept,
so the oracle is not toy-os. Two images, each on an emulated USB stick:

  1. HOST-MADE: `mkfs.exfat` on the host; the guest mounts it, makes a
     directory, copies a file several clusters long, writes a file with a
     non-ASCII name, renames one, deletes one, truncates one, unmounts.
  2. GUEST-MADE: a blank stick the guest formats itself (`mkfs -t exfat`),
     then writes to.

After the guest is gone, each image must pass `fsck.exfat -n` (the
checksums, the bitmap against every chain, the up-case table), and a
reader written HERE from the specification -- sharing no code with the
kernel -- must find exactly the files the guest left, byte for byte and
name for name, and not the ones it deleted.

What a broken version would still pass: a guest that only reads back its
own writes agrees with itself; only another implementation can say the
entry-set checksum, the name hash or a NoFatChain run is right.

    python3 tools/exfat_hostcheck.py
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import Results, copy_disk, poll  # noqa: E402

TOOL = "exfat_hostcheck"
BIG = os.path.join(REPO, "seed", "sync", "bin", "dash")
UNAME = "Käyttäjä-ÄÖ-ž.txt"   # no spaces: the shell has no quoting for `write`


# ---- an independent exFAT reader (the specification, sections 3, 6, 7) ----

class Exfat:
    def __init__(self, path):
        self.f = open(path, "rb")
        b = self.f.read(512)
        assert b[3:11] == b"EXFAT   ", "no exFAT boot sector"
        self.bps = 1 << b[108]
        self.cbytes = self.bps << b[109]
        self.fat_off, self.fat_len, self.heap, self.count, self.root = struct.unpack_from("<IIIII", b, 80)
        self.upcase = list(range(65536))
        raw = self.data(self.root, False, 1 << 40)
        for i in range(0, len(raw), 32):
            if raw[i] == 0:
                break
            if raw[i] == 0x82:   # the up-case table (7.2), decompressed
                fc, ln = struct.unpack_from("<IQ", raw, i + 20)
                t = self.data(fc, False, ln)
                c = k = 0
                while k + 1 < len(t) and c < 65536:
                    v = struct.unpack_from("<H", t, k)[0]
                    if v == 0xFFFF and k + 3 < len(t):
                        c += struct.unpack_from("<H", t, k + 2)[0]
                        k += 4
                        continue
                    self.upcase[c] = v
                    c += 1
                    k += 2
        self.hash_errors = []

    def name_hash(self, name):
        h = 0
        for u in struct.unpack(f"<{len(name.encode('utf-16-le')) // 2}H", name.encode("utf-16-le")):
            u = self.upcase[u]
            for byte in (u & 0xFF, u >> 8):
                h = (((h & 1) << 15) + (h >> 1) + byte) & 0xFFFF
        return h

    def read(self, sector, n=1):
        self.f.seek(sector * self.bps)
        return self.f.read(n * self.bps)

    def fat(self, c):
        self.f.seek(self.fat_off * self.bps + c * 4)
        return struct.unpack("<I", self.f.read(4))[0]

    def clusters(self, first, nofat, length):
        if not first:
            return []
        if nofat:
            return list(range(first, first + (length + self.cbytes - 1) // self.cbytes))
        out, c = [], first
        while 2 <= c < self.count + 2 and len(out) <= self.count:
            out.append(c)
            c = self.fat(c)
        return out

    def data(self, first, nofat, length, valid=None):
        buf = b""
        for c in self.clusters(first, nofat, length):
            buf += self.read(self.heap + (c - 2) * (self.cbytes // self.bps), self.cbytes // self.bps)
        buf = buf[:length]
        if valid is not None and valid < length:
            buf = buf[:valid] + b"\0" * (length - valid)
        return buf

    def entries(self, first, nofat, length):
        """Every in-use file set in a directory: (name, attr, first, nofat, length, valid)."""
        raw = self.data(first, nofat, length or 1 << 40)
        out, i = [], 0
        while i + 32 <= len(raw):
            t = raw[i]
            if t == 0:
                break
            if t != 0x85:
                i += 32
                continue
            n = raw[i + 1] + 1
            s = raw[i:i + 32 * n]
            want = struct.unpack_from("<H", s, 2)[0]
            got = 0
            for k, byte in enumerate(s):
                if k in (2, 3):
                    continue
                got = (((got & 1) << 15) + (got >> 1) + byte) & 0xFFFF
            assert got == want, f"set checksum {got:#x} != {want:#x}"
            attr = struct.unpack_from("<H", s, 4)[0]
            st = s[32:64]
            nlen = st[3]
            name = b"".join(s[32 * k + 2:32 * k + 32] for k in range(2, n))[:nlen * 2].decode("utf-16-le")
            if struct.unpack_from("<H", st, 4)[0] != self.name_hash(name):
                self.hash_errors.append(name)
            valid, = struct.unpack_from("<Q", st, 8)
            fc, ln = struct.unpack_from("<IQ", st, 20)
            out.append((name, attr, fc, bool(st[1] & 2), ln, valid))
            i += 32 * n
        return out

    def find(self, path):
        first, nofat, length, entry = self.root, False, 0, None
        for part in [p for p in path.split("/") if p]:
            hit = [e for e in self.entries(first, nofat, length) if e[0].upper() == part.upper()]
            if not hit:
                return None
            entry = hit[0]
            first, nofat, length = entry[2], entry[3], entry[4]
        return entry

    def file(self, path):
        e = self.find(path)
        return None if e is None else self.data(e[2], e[3], e[4], e[5])


# `write` ends the text with a newline, as echo does.

# ---- the guest ----------------------------------------------------------

class Guest:
    def __init__(self, instance, disk, sticks):
        self.base = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(instance)]
        args = self.base + ["--disk", disk, "--mem", "1024"]
        for s in sticks:
            args += ["--usb-stick", s]
        r = subprocess.run(args + ["start"], cwd=REPO, capture_output=True, text=True)
        self.ok, self.why = r.returncode == 0, (r.stdout + r.stderr).strip()[-300:]

    def run(self, *cmds, timeout=120):
        return subprocess.run(self.base + ["--timeout", str(timeout), "exec", *cmds], cwd=REPO,
                              capture_output=True, text=True).stdout

    def stop(self):
        subprocess.run(self.base + ["stop"], cwd=REPO, capture_output=True)


def fsck(res, label, img):
    r = subprocess.run(["fsck.exfat", "-n", img], capture_output=True, text=True)
    res.check(f"{label}: fsck.exfat -n finds nothing wrong", r.returncode == 0,
              (r.stdout + r.stderr).strip().replace("\n", " | ")[-300:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=9)
    a = ap.parse_args()
    for t in ("mkfs.exfat", "fsck.exfat"):
        if not shutil.which(t):
            print(f"{TOOL}: needs {t} (exfatprogs)")
            return 1
    res = Results()
    tmp = tempfile.mkdtemp(prefix="exfathost.")
    g = None
    big = open(BIG, "rb").read()
    try:
        disk = os.path.join(tmp, "disk.img")
        copy_disk(os.path.join(REPO, "disk.img"), disk, cwd=REPO)
        host_img = os.path.join(tmp, "host.img")
        with open(host_img, "wb") as f:
            f.truncate(256 << 20)
        subprocess.run(["mkfs.exfat", "-L", "HOSTMADE", host_img], capture_output=True, check=True)
        guest_img = os.path.join(tmp, "guest.img")
        with open(guest_img, "wb") as f:
            f.truncate(128 << 20)

        g = Guest(a.instance, disk, [host_img, guest_img])
        if not res.check("the guest boots with both sticks", g.ok, g.why):
            return res.finish(TOOL)
        # automountd took it at /media/HOSTMADE; stopped, so the stick is
        # this test's alone and its writes go where the checks look.
        poll(lambda: "/media/HOSTMADE" in g.run("mount"), 15)
        g.run("service stop automountd", "umount /media/HOSTMADE")
        out = g.run("mkdir /mnt/h", "mount usb0 /mnt/h", "mount")
        res.check("the host-made exFAT stick mounts as exfat",
                  any("/mnt/h" in ln and "exfat" in ln for ln in out.splitlines()),
                  out.strip()[-300:])
        g.run("mkdir /mnt/h/Kansio", "cp /bin/dash /mnt/h/Kansio/dash.bin",
              f"write /mnt/h/{UNAME} hei maailma",
              "write /mnt/h/gone.txt delete-me", "rm /mnt/h/gone.txt",
              "write /mnt/h/old.txt renamed", "mv /mnt/h/old.txt /mnt/h/new.txt",
              "cp /bin/dash /mnt/h/cut.bin", "truncate /mnt/h/cut.bin 1000")
        back = g.run("sum /mnt/h/Kansio/dash.bin")
        res.check("the guest reads back what it wrote",
                  f"{zlib.crc32(big)} {len(big)}" in back, back.strip()[-200:])

        g.run("mkfs -t exfat usb1 confirm", "mkdir /mnt/g", "mount usb1 /mnt/g",
              "cp /bin/dash /mnt/g/dash.bin", "write /mnt/g/x.txt guest-made")
        g.run("umount /mnt/h", "umount /mnt/g")
        g.stop()
        g = None

        fsck(res, "host-made", host_img)
        x = Exfat(host_img)
        res.check("host-made: a file several clusters long reads back byte for byte",
                  x.file("Kansio/dash.bin") == big,
                  f"{len(x.file('Kansio/dash.bin') or b'')} bytes, want {len(big)}")
        res.check("host-made: the non-ASCII name is the name another reader sees",
                  x.file(UNAME) == b"hei maailma\n", repr(x.file(UNAME)))
        res.check("host-made: the renamed file is there, the old and deleted names are not",
                  x.file("new.txt") == b"renamed\n" and x.find("old.txt") is None and
                  x.find("gone.txt") is None, "")
        res.check("host-made: every name's hash is the one its up-case table gives",
                  not x.hash_errors, ", ".join(x.hash_errors))
        cut = x.file("cut.bin")
        res.check("host-made: truncate kept the first 1000 bytes", cut == big[:1000],
                  f"{len(cut or b'')} bytes")

        fsck(res, "guest-made", guest_img)
        y = Exfat(guest_img)
        y.file("x.txt")
        res.check("guest-made: every name's hash is the one its own up-case table gives",
                  not y.hash_errors, ", ".join(y.hash_errors))
        res.check("guest-made: what the guest wrote on its own format reads back",
                  y.file("dash.bin") == big and y.file("x.txt") == b"guest-made\n",
                  f"dash {len(y.file('dash.bin') or b'')} bytes, x.txt {y.file('x.txt')!r}")
    finally:
        if g:
            g.stop()
        shutil.rmtree(tmp, ignore_errors=True)
    return res.finish(TOOL)


if __name__ == "__main__":
    sys.exit(main())
