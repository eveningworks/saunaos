#!/usr/bin/env python3
"""The kernel's removable-media parsers against a hostile stick, on the host.

WHAT IS UNDER TEST. A stick is mounted the moment it is plugged in
(automountd), so every byte FAT32, exFAT, ISO 9660, the partition table
and fs_ident read is the stick's to choose, and is parsed in ring 0.
Their real sources (kernel/fs/fat32*.c, exfat*.c, iso9660.c, fs_ident.c,
kernel/drivers/partition.c) are compiled with the host gcc under
AddressSanitizer and UBSan beside mediafuzz/: a shim for the kernel
services they call over a sparse in-memory disk, and a driver that
probes, mounts, lists, stats and reads everything, then writes, checks
or shrinks as a case asks. Besides the sanitizers it holds the CONTRACT
(mediafuzz.c's bad()): a listed name is one path component, a volume's
usage is no more than its size.

  - SEEDS: real images from the host's mkfs.fat + mtools, xorriso
    (plain and Joliet), sfdisk (MBR, GPT), and exFAT from the kernel's
    own format -- all of which must mount and list
  - CRAFTED IMAGES, one per audit finding (2026-11-12), each with the
    expectation it broke: a looping directory chain that never ends, a
    FAT size wrapping 32 bits, a directory at cluster 1 writing into the
    FAT, a 2 GiB size on a one-cluster file zero-filling the volume, a
    '/' in a name, a shrink queueing a directory that holds itself, GPT
    entries smaller than 128 bytes, an NTFS label walk wrapping
  - MUTATIONS of the seeds, every one parsed or refused cleanly

What a broken version would still pass: a parser that mounts NOTHING
passes the crafted cases, so every seed must mount and list its files.

POSITIVE CONTROL (--positive-control): the directory walk's bound, the
NTFS walk's 64-bit compare and the GPT entry-size check come out of a
copy of the sources -- their crafted cases must go red.

`--fuzz SECONDS` runs libFuzzer (clang) over the same harness, the seeds
and crafted cases as its corpus -- by hand: a first 10-minute run found
nothing the reviewers had not.

    python3 tools/hostchecks/mediaparse_hostcheck.py [--positive-control] [--iters N]
    python3 tools/hostchecks/mediaparse_hostcheck.py --fuzz 600 [--jobs 8] [--keep DIR]
"""

# FAT32/exFAT/ISO 9660/partition/fs_ident sources over seeds, crafted
# images and mutations, under ASan + UBSan. ~30 s.
RUNNER = {"covers": "removable-media parsers: seeds, crafted images, mutations", "gate": True,
          "needs": ["host_mkfs", "needs gcc, mkfs.fat, mtools, xorriso and sfdisk on PATH"]}

import argparse
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mediafuzz")
TOOL = "mediaparse_hostcheck"
SAN = ("-O1", "-g", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")
KERNEL = ["kernel/fs/fat32.c", "kernel/fs/fat32_resize.c", "kernel/fs/exfat.c", "kernel/fs/exfat_write.c",
          "kernel/fs/exfat_format.c", "kernel/fs/iso9660.c", "kernel/fs/fs_ident.c", "kernel/drivers/partition.c",
          "kernel/lib/string.c", "kernel/lib/caltime.c", "kernel/lib/kcrc.c", "kernel/lib/kfmt.c",
          "kernel/lib/kfmt_nofloat.c", "kernel/lib/knum.c", "kernel/lib/kpath_buf.c", "kernel/lib/kpath.c"]
CONTROL = [
    ("kernel/fs/fat32.c", "if (++w->hops >= FAT_DIR_MAX_BYTES / (sbi->v.sectors_per_cluster * SECTOR)) {",
     "if (0) {"),
    ("kernel/fs/fs_ident.c", "len < 24 || (uint64_t)a + len > rec", "len < 24 || a + len > rec"),
    ("kernel/drivers/partition.c",
     "if (entry_size < 128 || entry_size > PART_SECTOR_SIZE || (entry_size & (entry_size - 1))) return 0;",
     "if (entry_size == 0 || entry_size > PART_SECTOR_SIZE) entry_size = 128;"),
]
OP_WRITE, OP_CHECK, OP_SHRINK, OP_TOUCH = 1, 2, 4, 8
LIMIT = 15   # seconds; a case still running is a loop


# ---- the sparse image the harness reads (mediafuzz.c's INPUT) ---------------
class Img:
    def __init__(self, ops=0, sectors=0):
        self.ops, self.sectors, self.s = ops, sectors, {}

    @classmethod
    def load(cls, path):
        d = open(path, "rb").read()
        img = cls(d[0], struct.unpack_from("<I", d, 1)[0])
        for o in range(5, len(d) - 4, 516):
            img.s[struct.unpack_from("<I", d, o)[0]] = bytearray(d[o + 4:o + 516].ljust(512, b"\0"))
        return img

    @classmethod
    def from_raw(cls, path, ops):
        data = open(path, "rb").read()
        img = cls(ops, len(data) // 512)
        for n in range(img.sectors):
            sec = data[n * 512:(n + 1) * 512]
            if any(sec):
                img.s[n] = bytearray(sec)
        return img

    def sec(self, n):
        return self.s.setdefault(n, bytearray(512))

    def read(self, off, n):
        out = bytearray()
        while n:
            s, i = divmod(off, 512)
            k = min(n, 512 - i)
            out += self.s.get(s, bytes(512))[i:i + k]
            off, n = off + k, n - k
        return bytes(out)

    def write(self, off, data):
        while data:
            s, i = divmod(off, 512)
            k = min(len(data), 512 - i)
            self.sec(s)[i:i + k] = data[:k]
            off, data = off + k, data[k:]

    def save(self, path):
        with open(path, "wb") as f:
            f.write(bytes([self.ops]) + struct.pack("<I", self.sectors))
            for n in sorted(self.s):
                if any(self.s[n]):
                    f.write(struct.pack("<I", n) + bytes(self.s[n]))


class Fat:
    def __init__(self, img):
        self.img = img
        b = img.sec(0)
        self.spc, self.res, self.nfats = b[13], struct.unpack_from("<H", b, 14)[0], b[16]
        self.fatsz, self.root = struct.unpack_from("<I", b, 36)[0], struct.unpack_from("<I", b, 44)[0]
        self.data = self.res + self.nfats * self.fatsz

    def set_fat(self, c, v):
        for k in range(self.nfats):
            self.img.write((self.res + k * self.fatsz) * 512 + c * 4, struct.pack("<I", v))

    def clus_off(self, c):
        return (self.data + (c - 2) * self.spc) * 512

    def find(self, c, short):
        base = self.clus_off(c)
        for i in range(self.spc * 512 // 32):
            e = self.img.read(base + i * 32, 32)
            if e[0] == 0:
                break
            if e[11] != 0x0F and e[0] != 0xE5 and e[:11].rstrip() == short.encode():
                return base + i * 32, e
        raise SystemExit("%s: no %r in the seed's root -- the seed changed, fix the case" % (TOOL, short))

    @staticmethod
    def cluster_of(e):
        return struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]

    def set_cluster(self, off, c):
        self.img.write(off + 20, struct.pack("<H", c >> 16))
        self.img.write(off + 26, struct.pack("<H", c & 0xFFFF))


# ---- seeds -------------------------------------------------------------------
def sh(*a, **k):
    subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **k)


def seeds(tmp, exe):
    out = os.path.join(tmp, "seeds")
    os.makedirs(out)
    tree = os.path.join(tmp, "tree")
    os.makedirs(os.path.join(tree, "docs", "deeper"))
    open(os.path.join(tree, "small.txt"), "w").write("abc")
    open(os.path.join(tree, "big.bin"), "wb").write(bytes(random.Random(1).randrange(256) for _ in range(9000)))
    open(os.path.join(tree, "docs", "deeper", "Käyttäjä with a long name.txt"), "w").write("hello")
    for i in range(20):
        open(os.path.join(tree, "docs", "f%02d.txt" % i), "w").write("f")
    for name, mb, spc in (("fat32_s1", 34, 1), ("fat32_s8", 260, 8)):
        img = os.path.join(tmp, name)
        sh("truncate", "-s", "%dM" % mb, img)
        sh("mkfs.fat", "-F", "32", "-s", str(spc), "-n", "FUZZ", img)
        sh("mcopy", "-s", "-i", img, os.path.join(tree, "small.txt"), os.path.join(tree, "big.bin"),
           os.path.join(tree, "docs"), "::/")
        Img.from_raw(img, OP_WRITE | OP_CHECK).save(os.path.join(out, name))
    for name, extra in (("iso_plain", []), ("iso_joliet", ["-joliet", "on", "-rockridge", "on"])):
        img = os.path.join(tmp, name + ".iso")
        sh("xorriso", "-outdev", img, *extra, "-map", tree, "/", "-commit")
        Img.from_raw(img, 0).save(os.path.join(out, name))
    sh(exe, "--make", "exfat", os.path.join(out, "exfat"), str(64 * 2048))
    for name, script in (("mbr", "label: dos\n,8M,c\n,8M,83\n,,5\n,4M,83\n,4M,83\n"),
                         ("gpt", "label: gpt\n,8M,U\n,8M,L\n,,L\n")):
        img = os.path.join(tmp, name)
        sh("truncate", "-s", "40M", img)
        subprocess.run(["sfdisk", "-q", img], input=script.encode(), check=True, stdout=subprocess.DEVNULL)
        Img.from_raw(img, 0).save(os.path.join(out, name))
    # What each seed must do, so a parser that mounts nothing cannot pass.
    want = {"fat32_s1": ("fat32", 25), "fat32_s8": ("fat32", 25), "exfat": ("exfat", 25),
            "iso_plain": ("iso", 25), "iso_joliet": ("iso", 25), "mbr": ("parts(3)", 0), "gpt": ("parts(3)", 0)}
    return out, want


# ---- crafted images, one per finding ----------------------------------------
def crafted(seed_dir):
    out = {}

    def fat(ops):
        img = Img.load(os.path.join(seed_dir, "fat32_s1"))
        img.ops = ops
        return img, Fat(img)

    img, f = fat(0)   # every entry deleted, the chain looping: no end marker, no chain end
    f.set_fat(f.root, f.root)
    img.write(f.clus_off(f.root), (b"\xe5" + b" " * 10 + b"\x20" + bytes(20)) * 16)
    out["fat_root_cycle"] = (img, {})

    img, f = fat(0)   # a live subdirectory looping: its twenty names, forever
    off, e = f.find(f.root, "DOCS")
    f.set_fat(Fat.cluster_of(e), Fat.cluster_of(e))
    out["fat_subdir_cycle"] = (img, {})

    img, f = fat(0)   # nfats * fatsz32 wraps: the data region inside the FAT
    img.write(36, struct.pack("<I", 0x80000010))
    out["fat_fatsz_wrap"] = (img, {"not_mounted": "fat32"})

    img, f = fat(OP_TOUCH)   # a directory at cluster 1: a create in it lands in the FAT
    off, e = f.find(f.root, "DOCS")
    f.set_cluster(off, 1)
    out["fat_dir_cluster1"] = (img, {"guard": [0, f.data]})

    img, f = fat(OP_WRITE)   # 2 GiB on one cluster (0xFFFFFFFF is refused by the 4 GiB limit first)
    off, e = f.find(f.root, "SMALL   TXT")
    img.write(off + 28, struct.pack("<I", 0x7FFF0000))
    out["fat_size_past_chain"] = (img, {"max_written": 4096})

    img, f = fat(0)   # a '/' in a short name
    off, e = f.find(f.root, "SMALL   TXT")
    img.write(off, b"A/B     TXT")
    out["fat_name_slash"] = (img, {})

    img, f = fat(OP_SHRINK)   # the root holding itself, and a shrink walking every directory
    off, e = f.find(f.root, "DOCS")
    loop = bytearray(e)
    loop[0:11] = b"LOOP       "
    loop[20:22] = struct.pack("<H", f.root >> 16)
    loop[26:28] = struct.pack("<H", f.root & 0xFFFF)
    for i in range(16):
        pos = f.clus_off(f.root) + i * 32
        if img.read(pos, 1)[0] == 0:
            img.write(pos, bytes(loop))
            break
    else:
        raise SystemExit("%s: no free slot in the seed's root -- fix fat_shrink_dir_cycle" % TOOL)
    out["fat_shrink_dir_cycle"] = (img, {"max_alloc": 1 << 20})

    img = Img.load(os.path.join(seed_dir, "gpt"))   # one-byte GPT entries, the header's CRC right
    img.ops = 0
    h = img.sec(1)
    hsz, arr = struct.unpack_from("<I", h, 12)[0], struct.unpack_from("<Q", h, 72)[0]
    struct.pack_into("<I", h, 84, 1)
    struct.pack_into("<I", h, 16, 0)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:hsz])))
    a = img.sec(arr)
    a[:] = bytes(512)
    a[511] = 1
    out["gpt_entry_size_1"] = (img, {})

    def ntfs(attrs):   # an NTFS boot sector and MFT record 3, as fs_ident reads the label
        img = Img(0, 2048)
        b = img.sec(0)
        b[3:11] = b"NTFS    "
        struct.pack_into("<H", b, 0x0B, 512)
        b[0x0D], b[0x40] = 8, 0xF6
        struct.pack_into("<Q", b, 0x30, 4)
        b[510:512] = b"\x55\xaa"
        rec = bytearray(1024)
        rec[0:4] = b"FILE"
        struct.pack_into("<HH", rec, 4, 0x30, 1)
        struct.pack_into("<H", rec, 0x14, 0x38)
        for at, fmt, vals in attrs:
            struct.pack_into(fmt, rec, at, *vals)
        img.write(4 * 4096 + 3 * 1024, bytes(rec))
        return img
    out["ntfs_attr_wrap"] = (ntfs([(0x38, "<II", (0x10, 0x30)), (0x68, "<II", (0x10, 0xFFFFFFD0))]), {})
    out["ntfs_label_wrap"] = (ntfs([(0x38, "<II", (0x60, 0x30)), (0x48, "<IH", (0xFFFFE010, 0x2000)),
                                    (0x68, "<I", (0xFFFFFFFF,))]), {})
    return out


# ---- running ----------------------------------------------------------------
def run(exe, path, exp=None, limit=LIMIT):
    """None, or why the input broke its expectation."""
    exp = exp or {}
    env = dict(os.environ, FZ_REPORT="1", ASAN_OPTIONS="detect_leaks=0")
    if "guard" in exp:
        env["FZ_GUARD"] = "%d:%d" % tuple(exp["guard"])
    try:
        p = subprocess.run([exe, path], capture_output=True, text=True, timeout=limit, env=env)
    except subprocess.TimeoutExpired:
        return "still running after %gs" % limit, ""
    out = p.stdout + p.stderr
    m = re.search(r"(INVARIANT: .*|ERROR: AddressSanitizer: \S+|runtime error: .*)", out)
    if m:
        return m.group(1), out
    if p.returncode:
        return "exit %d" % p.returncode, out
    report = p.stdout.split(":", 1)[1] if ":" in p.stdout else ""
    if exp.get("not_mounted") and (" %s " % exp["not_mounted"]) in report:
        return "mounted " + exp["not_mounted"], out
    for key, pat, what in (("max_alloc", r"biggest alloc (\d+)", "asked the heap for %s bytes"),
                           ("max_written", r"wrote (\d+)", "wrote %s sectors")):
        got = re.search(pat, report)
        if key in exp and got and int(got.group(1)) > exp[key]:
            return what % got.group(1), out
    return None, out


def mutate(rng, src, dst):
    d = bytearray(open(src, "rb").read())
    for _ in range(rng.randint(1, 12)):
        if len(d) <= 5:
            break
        at = rng.randrange(5, len(d))
        d[at] = rng.choice((0, 1, 0xFF, 0x7F, 0x80, rng.randrange(256)))
    open(dst, "wb").write(bytes(d))


def build(tmp, control, cc="gcc", libfuzzer=False):
    gen = os.path.join(tmp, "gen")
    os.makedirs(gen, exist_ok=True)
    sh("sh", os.path.join(ROOT, "tools/build/gen_kconfig.sh"), "1000", "idle", "1", os.path.join(gen, "kconfig.h"))
    srcs = []
    for rel in KERNEL:
        edits = [(a, b) for f, a, b in CONTROL if f == rel]
        # Staged beside a copy only when edited; a staged file finds its
        # neighbours' headers through -iquote on its own directory.
        srcs.append(hostcheck.stage(tmp, rel, dest=rel, edits=edits, apply=control, tool=TOOL)
                    if edits and control else os.path.join(ROOT, rel))
    inc = [HERE] + [os.path.join(ROOT, "kernel/include", d) for d in ("api", "abi", "kernel")] + \
          [os.path.join(ROOT, "kernel/fs"), os.path.join(ROOT, "kernel/drivers"), gen]
    flags = list(SAN) + [x for i in inc for x in ("-iquote", i)]
    if libfuzzer:
        flags = ["-fsanitize=fuzzer", "-DFZ_LIBFUZZER"] + flags
    exe = os.path.join(tmp, "mediafuzz_lf" if libfuzzer else "mediafuzz")
    r = subprocess.run([cc, *flags, "-o", exe, os.path.join(HERE, "mediafuzz.c"), os.path.join(HERE, "media_shim.c"),
                        *srcs], capture_output=True, text=True)
    if r.returncode:
        sys.exit("%s: compile failed:\n%s" % (TOOL, r.stderr[-3000:]))
    return exe


def fuzz(args, tmp, exe, corpus):
    if not shutil.which("clang"):
        sys.exit("%s: --fuzz needs clang (libFuzzer)" % TOOL)
    lf = build(tmp, False, cc="clang", libfuzzer=True)
    keep = args.keep or os.path.join(tmp, "fuzz")
    for d in ("corpus", "crashes"):
        os.makedirs(os.path.join(keep, d), exist_ok=True)
    for f in os.listdir(corpus):
        shutil.copy(os.path.join(corpus, f), os.path.join(keep, "corpus", f))
    # -jobs/-workers, never -fork: libFuzzer's fork mode crashed in itself
    # (clang 23) before reaching the target.
    subprocess.run([lf, "-jobs=%d" % args.jobs, "-workers=%d" % args.jobs, "-timeout=%d" % LIMIT,
                    "-rss_limit_mb=3000", "-max_len=120000", "-max_total_time=%d" % args.fuzz,
                    "-artifact_prefix=%s/" % os.path.join(keep, "crashes"), os.path.join(keep, "corpus")],
                   cwd=keep, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    found = sorted(os.listdir(os.path.join(keep, "crashes")))
    print("%s: %d second(s) of libFuzzer, %d artifact(s)%s" % (TOOL, args.fuzz, len(found),
          "" if not found else " in " + os.path.join(keep, "crashes")))
    for f in found[:20]:
        why, _ = run(exe, os.path.join(keep, "crashes", f))
        print("  %s: %s" % (f, why or "does not reproduce in the replay build"))
    return 1 if found else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    ap.add_argument("--iters", type=int, default=200, help="seed mutations (default 200)")
    ap.add_argument("--fuzz", type=int, metavar="SECONDS", help="run libFuzzer that long instead")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() // 2 or 1, help="libFuzzer workers")
    ap.add_argument("--keep", metavar="DIR", help="where --fuzz keeps its corpus and crashes")
    args = ap.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="mediaparse-") as tmp:
        exe = build(tmp, args.positive_control)
        seed_dir, want = seeds(tmp, exe)
        cases = crafted(seed_dir)
        cdir = os.path.join(tmp, "crafted")
        os.makedirs(cdir)
        for name, (img, _exp) in cases.items():
            img.save(os.path.join(cdir, name))
        if args.fuzz:
            corpus = os.path.join(tmp, "corpus")
            os.makedirs(corpus)
            for d in (seed_dir, cdir):
                for f in os.listdir(d):
                    shutil.copy(os.path.join(d, f), os.path.join(corpus, f))
            return fuzz(args, tmp, exe, corpus)

        print("seeds:")
        for name in sorted(want):
            why, out = run(exe, os.path.join(seed_dir, name))
            what, n = want[name]
            got = re.search(r"(\d+) entries", out)
            if not why and (" %s" % what not in out.split(":", 1)[1] or not got or int(got.group(1)) < n):
                why = "did not mount and list as it should: " + out.strip().splitlines()[-1]
            print("  %-22s %s" % (name, "RED  " + why if why else "green"))
            if why:
                failures.append(name)
        print("crafted:")
        for name, (_img, exp) in sorted(cases.items()):
            why, _ = run(exe, os.path.join(cdir, name), exp)
            print("  %-22s %s" % (name, "RED  " + why if why else "green"))
            if why:
                failures.append(name)
        rng = random.Random(20261112)
        bad = 0
        names = sorted(want)
        mut = os.path.join(tmp, "mut")
        for i in range(args.iters):
            mutate(rng, os.path.join(seed_dir, names[i % len(names)]), mut)
            why, _ = run(exe, mut)
            if why:
                bad += 1
                kept = os.path.join(tempfile.gettempdir(), "mediaparse-red-%d" % i)
                shutil.copy(mut, kept)
                if bad <= 3:
                    print("  mutation of %s: RED  %s (kept as %s)" % (names[i % len(names)], why, kept))
        print("mutations: %d, %d red" % (args.iters, bad))
        if bad:
            failures.append("mutations")
    if args.positive_control:
        print("\n%s: POSITIVE CONTROL -- %d check(s) went red%s" %
              (TOOL, len(failures), "" if failures else " -- NONE: the check cannot see the break"))
        return 0 if failures else 1
    print("\n%s: %s" % (TOOL, "PASS" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
