#!/usr/bin/env python3
"""Host-side: does tfs3_writer.py's OVERWRITE hand every old block back?

Does a short last group's bitmap mark the blocks past the volume's end
in use, as the kernel's own format does? And does a HARD LINK in the seed tree land as one on the image? `cc` and
`gcc` (`make native-toolchain`) are one file on the host; sync must give
them ONE inode with two links, keep that across a re-sync, split them
when the tree does, and free nothing a remaining name still uses.

Rewriting a file is delete-then-write (`write_file()`), so the blocks
the old copy held must all return to the bitmap. If any do not, the
image gains an allocated block nothing references -- a LEAK, which the
kernel's own `fsck` reports and which accumulates one pair per rewrite.

WHY THE SIZES ARE THE TEST. TFS3's block map is twelve direct pointers,
then single-, double- and triple-indirect tables, and a free path can be
correct for one level and wrong for the next. A fixture that only ever
writes a small file cannot see that: the seed tree has exactly ONE file
over the single-indirect ceiling (/install/kernel.bin, ~4.8 MB), which
is why a real leak sat in the seeding path with every existing test
green. So each size below is chosen to reach one more level than the
one before it, and the double-indirect case is the one that was broken.

Triple-indirect is NOT covered: it starts past 4 GiB, and a test image
that large is not worth the disk. `tfs3_writer.py`'s own write path
marks that level unexercised for the same reason.

No VM: this reads the allocation bitmap directly, so it runs in about a
second and needs nothing built.
"""

# The host-side TFS3 writer is what SEEDS disk.img, so a leak in it
# corrupts this gate's own fixture -- an overwrite that did not free its
# double-indirect tables reddened ktest's fsck checks on the second run
# against one image. Host-only and about a second; no VM, nothing built.
RUNNER = {"covers": "the host seeder returns every block it frees, seeds hard links as links, and marks a short last group's tail in use"}

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from tfs3_writer import Tfs3Image, BLOCK, BLOCKS_PER_GROUP  # noqa: E402

TOOL = toolpath.tool("tfs3_writer.py")
IMG_MB = 96

# (label, bytes) -- each one reaches a level the previous did not.
# 12 direct blocks = 48 KiB; + 1024 single-indirect = 4,243,456 bytes.
CASES = [
    ("direct only",      40 * 1024),
    ("single-indirect",  1024 * 1024),
    ("double-indirect",  4_814_512),   # the size of the real kernel.bin
]

g_fail = 0


def check(ok, what):
    global g_fail
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        g_fail += 1


def run(*args):
    subprocess.run([sys.executable, TOOL, *args], check=True,
                   stdout=subprocess.DEVNULL)


def allocated(path):
    """How many blocks the bitmap says are in use."""
    img = Tfs3Image(path)
    n = 0
    for g in range(img.sb["gc"]):
        span = img.group_span(g)
        if not span:
            continue
        bm = img.read_block(img.group_base(g))
        n += sum((bm[i >> 3] >> (i & 7)) & 1 for i in range(span))
    img.close()
    return n


def node(path, dpath):
    img = Tfs3Image(path)
    ino = img.lookup(dpath)
    n = img.read_inode(ino) if ino is not None else None
    data = img.read_file_data(ino) if ino is not None else None
    img.close()
    return ino, (n or {}).get("links"), data


def hard_links(tmp):
    print("tfs3_writer_test: a hard link in the seed tree is one on the image")
    img = os.path.join(tmp, "l.img")
    seed = os.path.join(tmp, "seed")
    bindir = os.path.join(seed, "sync", "usr", "bin")
    os.makedirs(bindir)
    gcc, cc = os.path.join(bindir, "gcc"), os.path.join(bindir, "cc")
    a, b = b"A" * (200 * 1024), b"B" * (300 * 1024)
    na, nb = (len(a) + BLOCK - 1) // BLOCK, (len(b) + BLOCK - 1) // BLOCK
    with open(gcc, "wb") as f:
        f.write(a)
    os.link(gcc, cc)
    run("format", img, "--size", str(IMG_MB * 1024 * 1024), "--force")
    base = allocated(img)
    run("sync", img, seed)
    linked = allocated(img)
    (gi, gl, gd), (ci, cl, cd) = node(img, "/usr/bin/gcc"), node(img, "/usr/bin/cc")
    check(gi == ci and gl == 2 and cl == 2, f"cc and gcc are one inode with two links (ino {gi}/{ci}, links {gl}/{cl})")
    check(gd == a and cd == a, "both names read the file's bytes")
    run("sync", img, seed)
    check(node(img, "/usr/bin/cc")[0] == gi and allocated(img) == linked,
          "a second sync keeps the link and allocates nothing")

    # The tree splits them: gcc is a new file, cc keeps the old bytes.
    os.unlink(gcc)
    with open(gcc, "wb") as f:
        f.write(b)
    run("sync", img, seed)
    (gi2, gl2, gd2), (ci2, cl2, cd2) = node(img, "/usr/bin/gcc"), node(img, "/usr/bin/cc")
    check(gi2 != ci2 and gl2 == 1 and cl2 == 1 and gd2 == b and cd2 == a,
          f"split by the tree: two inodes, one link each, each its own bytes (links {gl2}/{cl2})")
    # 75 blocks is past the 12 direct pointers: one single-indirect table.
    check(allocated(img) == linked + nb + 1,
          f"the split frees nothing cc still reads -- allocated {allocated(img)}, want {linked + nb + 1}")

    # Linked again: cc's old copy is freed outright.
    os.unlink(cc)
    os.link(gcc, cc)
    run("sync", img, seed)
    (gi3, gl3, _), (ci3, cl3, cd3) = node(img, "/usr/bin/gcc"), node(img, "/usr/bin/cc")
    check(gi3 == ci3 and gl3 == 2 and cd3 == b, f"linked again: one inode, two links (links {gl3}/{cl3})")
    check(allocated(img) == linked - na + nb,
          f"the old copy is freed -- allocated {allocated(img)}, want {linked - na + nb}")

    # One name pruned: the other keeps the inode and every block.
    os.unlink(cc)
    with open(os.path.join(seed, "exact"), "w") as f:
        f.write("/usr/bin\n")
    before = allocated(img)
    run("sync", img, seed)
    gi4, gl4, gd4 = node(img, "/usr/bin/gcc")
    ci4 = node(img, "/usr/bin/cc")[0]
    check(ci4 is None and gi4 == gi3 and gl4 == 1 and gd4 == b,
          f"pruning cc leaves gcc whole with one link (cc ino {ci4}, gcc ino {gi3} -> {gi4}, links {gl4})")
    check(allocated(img) == before, f"...and frees nothing -- allocated {before} -> {allocated(img)}")
    _ = base


def tail_padding(tmp):
    """The last group is SHORT; its bitmap's bits past the volume's end
    must read in use, as the kernel's t3_bbm_default() writes them --
    fsck counts a clear one as "referenced but free"."""
    print("tfs3_writer_test: a short last group's bits past the end are in use")
    img = os.path.join(tmp, "p.img")
    run("format", img, "--size", str(IMG_MB * 1024 * 1024), "--force")
    t = Tfs3Image(img)
    g = t.sb["gc"] - 1
    span = t.group_span(g)
    bm = t.read_block(t.group_base(g))
    t.close()
    clear = sum(1 for i in range(span, BLOCKS_PER_GROUP)
                if not (bm[i >> 3] >> (i & 7)) & 1)
    check(span < BLOCKS_PER_GROUP, f"the fixture's last group is short ({span} of {BLOCKS_PER_GROUP})")
    check(clear == 0, f"every bit past the volume's end is set ({clear} clear)")


def main():
    print("tfs3_writer_test: an overwrite returns every block it held")
    with tempfile.TemporaryDirectory() as tmp:
        img = os.path.join(tmp, "t.img")
        run("format", img, "--size", str(IMG_MB * 1024 * 1024), "--force")

        for label, size in CASES:
            a = os.path.join(tmp, "a.bin")
            b = os.path.join(tmp, "b.bin")
            with open(a, "wb") as f:
                f.write(b"A" * size)
            with open(b, "wb") as f:
                f.write(b"B" * size)   # same size, different content
            dst = "/f_%d" % size

            run("write", img, a, dst)
            before = allocated(img)
            run("write", img, b, dst)
            after = allocated(img)

            blocks = (size + BLOCK - 1) // BLOCK
            check(after == before,
                  f"{label} ({blocks} blocks): rewrite leaks nothing "
                  f"-- allocated {before} -> {after}")

        hard_links(tmp)
        tail_padding(tmp)

    print("tfs3_writer_test: " + ("all checks passed" if not g_fail else "FAILURES"))
    return g_fail


sys.exit(main())
