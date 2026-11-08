#!/usr/bin/env python3
"""Host-side: is the SDK on the disk the tree's, and can a program be built from it alone?

No VM. Reads build/sysroot (`make sdk`) and disk.img's /usr/include and /usr/lib
(`make iso`), and builds a scratch image of its own for the pruning check.

WHAT A BROKEN VERSION WOULD STILL PASS, and what is asserted instead:
  - "the files exist" passes a stale disk: every file in build/sysroot is compared
    BYTE FOR BYTE with a fresh staging from the tree, and the image's two directories
    with build/sysroot -- in BOTH directions, so a header the tree deleted that is
    still on the image fails (docs/cc-design.md: a program compiled against stale
    headers, with nothing to say so);
  - "the headers are there" passes an SDK that only compiles with the tree's -I
    flags: a program using stdio, malloc, setjmp, varargs, errno, unistd and a
    toolkit header is compiled with `-nostdinc` against the sysroot ALONE and linked
    from its /usr/lib alone, and must come out ET_EXEC with _start at the entry;
  - "sync prunes" passes a prune that deletes nothing, or everything: on a scratch
    image, a file dropped from an `exact` directory goes, a nested directory dropped
    from it goes, a file kept in it stays, and a file outside it stays.

  python3 tools/tests/gate/sdk_test.py
  python3 tools/tests/gate/sdk_test.py --positive-control   # a tampered SDK: must fail
"""

# The SDK is a copy of the tree's interface on the disk, and a copy goes stale
# silently. Host-only, a few seconds.
RUNNER = {"covers": "the disk's /usr/include + /usr/lib match the tree, and build a program alone"}

import argparse
import filecmp
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
REPO = toolpath.REPO
import mkpart_test                                   # noqa: E402
import stage_sdk                                     # noqa: E402
import tfs3_writer as t3                             # noqa: E402

SDK_DIRS = ("usr/include", "usr/lib")

PROGRAM = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include "base/ufile.h"
static jmp_buf jb;
static int sum(int n, ...) { va_list ap; va_start(ap, n); int s = 0; while (n--) s += va_arg(ap, int); va_end(ap); return s; }
int main(void) {
    char *p = malloc(16);
    if (!p) return 1;
    strcpy(p, "sdk");
    if (!setjmp(jb)) longjmp(jb, 1);
    printf("%s %d %s %d\n", p, sum(3, 1, 2, 3), strerror(ENOENT), isatty(1));
    free(p);
    return 0;
}
"""


def tree_files(root):
    """{relative path: absolute path} for every file under root/<SDK_DIRS>."""
    out = {}
    for sub in SDK_DIRS:
        for d, _, files in os.walk(os.path.join(root, sub)):
            for f in files:
                p = os.path.join(d, f)
                out[os.path.relpath(p, root)] = p
    return out


def compare(want, got):
    """[problem] between two {rel: path} maps, both directions."""
    bad = [f"missing: /{k}" for k in sorted(set(want) - set(got))]
    bad += [f"not in the tree: /{k}" for k in sorted(set(got) - set(want))]
    bad += [f"different bytes: /{k}" for k in sorted(set(want) & set(got))
            if not filecmp.cmp(want[k], got[k], shallow=False)]
    return bad


def image_files(disk, out):
    """Copy the image's SDK directories under `out`; None if the image has no TFS3."""
    base, sectors = mkpart_test.volume_of(disk)
    img = t3.Tfs3Image(disk, writable=False, base_lba=base, sectors=sectors)
    try:
        def walk(path):
            ino = img.lookup(path)
            if ino is None:
                return
            for name, child in img.dirents(ino):
                if name in (".", ".."):
                    continue
                sub = path + "/" + name
                if img.read_inode(child)["type"] == t3.TYPE_DIR:
                    walk(sub)
                else:
                    dst = os.path.join(out, t3.host_path(sub).lstrip("/"))
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, "wb") as f:
                        f.write(img.read_file_data(child))
        for sub in SDK_DIRS:
            walk("/" + sub)
    finally:
        img.close()
    return tree_files(out)


def build_alone(sysroot, tmp):
    """(ok, detail): compile and link PROGRAM from the sysroot and nothing else."""
    src = os.path.join(tmp, "p.c")
    obj, exe = src[:-2] + ".o", src[:-2]
    with open(src, "w") as f:
        f.write(PROGRAM)
    gcc_inc = subprocess.run(["gcc", "-print-file-name=include"], capture_output=True,
                             text=True).stdout.strip()
    inc, lib = os.path.join(sysroot, "usr/include"), os.path.join(sysroot, "usr/lib")
    cc = ["gcc", "-std=gnu11", "-ffreestanding", "-nostdinc", "-isystem", gcc_inc,
          "-isystem", inc, "-idirafter", os.path.join(inc, "saunaos"),
          "-fstack-protector-strong", "-mstack-protector-guard=global", "-fpie",
          "-mno-direct-extern-access", "-Wa,-mrelax-relocations=no", "-mno-red-zone",
          "-ftls-model=local-exec", "-O2", "-Wall", "-Werror", "-c", src, "-o", obj]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode != 0:
        return False, "compile: " + r.stderr.strip()[-400:]
    rt = [os.path.join(lib, o) for o in stage_sdk.RT_OBJS]
    ld = ["ld", "-n", "--gc-sections", "--eh-frame-hdr", "-T", os.path.join(lib, "saunaos.ld"), "-nostdlib",
          "-o", exe, rt[0], obj, *rt[1:], os.path.join(lib, "libbase.a"), os.path.join(lib, "libc.a")]
    r = subprocess.run(ld, capture_output=True, text=True)
    if r.returncode != 0:
        return False, "link: " + r.stderr.strip()[-400:]
    with open(exe, "rb") as f:
        head = f.read(32)
    e_type, = struct.unpack_from("<H", head, 16)
    e_entry, = struct.unpack_from("<Q", head, 24)
    nm = subprocess.run(["nm", exe], capture_output=True, text=True).stdout
    start = [ln.split()[0] for ln in nm.splitlines() if ln.endswith(" _start")]
    if e_type != 2:
        return False, f"e_type {e_type}, not ET_EXEC"
    if not start or int(start[0], 16) != e_entry:
        return False, f"entry {e_entry:#x} is not _start ({start})"
    return True, ""


def prune_checks(tmp, check):
    seed = os.path.join(tmp, "seed")
    sync = os.path.join(seed, "sync")
    for p in ("usr/include/keep.h", "usr/include/gone.h", "usr/include/old/x.h", "etc/outside.txt"):
        os.makedirs(os.path.dirname(os.path.join(sync, p)), exist_ok=True)
        with open(os.path.join(sync, p), "w") as f:
            f.write(p + "\n")
    disk = os.path.join(tmp, "prune.img")
    with open(disk, "wb") as f:
        f.truncate(160 * 1024 * 1024)
    r = subprocess.run([sys.executable, toolpath.tool("seed_disk.py"), disk, seed],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("sdk_test: could not seed the scratch image:\n" + r.stdout + r.stderr)
    os.remove(os.path.join(sync, "usr/include/gone.h"))
    shutil.rmtree(os.path.join(sync, "usr/include/old"))
    os.remove(os.path.join(sync, "etc/outside.txt"))
    with open(os.path.join(seed, "exact"), "w") as f:
        f.write("# test\n/usr/include\n")
    r = subprocess.run([sys.executable, toolpath.tool("seed_disk.py"), disk, seed],
                       capture_output=True, text=True)
    check("a re-seed with an exact list succeeds", r.returncode == 0, (r.stdout + r.stderr)[-300:])
    base, sectors = mkpart_test.volume_of(disk)
    img = t3.Tfs3Image(disk, writable=False, base_lba=base, sectors=sectors)
    try:
        check("a file dropped from an exact directory is pruned", img.lookup("/usr/include/gone.h") is None)
        check("a directory dropped from it is pruned", img.lookup("/usr/include/old") is None)
        check("a file still in it is kept", img.lookup("/usr/include/keep.h") is not None)
        check("a file outside it is never pruned", img.lookup("/etc/outside.txt") is not None)
    finally:
        img.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--build", default="build")
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--positive-control", action="store_true",
                    help="tamper with a copy of the SDK: every comparison must fail")
    args = ap.parse_args()
    bad = []

    def check(name, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"   [{detail}]"))
        if not ok:
            bad.append(name)

    sysroot = os.path.join(REPO, args.build, "sysroot")
    if not os.path.isdir(os.path.join(sysroot, "usr/include")):
        sys.exit(f"sdk_test: {sysroot} has no SDK -- run `make sdk` (part of `make all`)")

    with tempfile.TemporaryDirectory() as tmp:
        if args.positive_control:
            copy = os.path.join(tmp, "tampered")
            shutil.copytree(sysroot, copy)
            with open(os.path.join(copy, "usr/include/stdio.h"), "a") as f:
                f.write("#error tampered\n")
            with open(os.path.join(copy, "usr/include/stale.h"), "w") as f:
                f.write("/* a header the tree deleted */\n")
            os.remove(os.path.join(copy, "usr/lib/crt0.o"))
            sysroot = copy

        fresh = os.path.join(tmp, "fresh")
        stage_sdk.stage(stage_sdk.plan(os.path.join(REPO, args.build), "gcc"), fresh, "objcopy")
        drift = compare(tree_files(fresh), tree_files(sysroot))
        check("build/sysroot is a fresh staging of the tree", not drift,
              "; ".join(drift[:4]) + (f" (+{len(drift) - 4})" if len(drift) > 4 else ""))

        ok, detail = build_alone(sysroot, tmp)
        check("a program compiles and links from the sysroot alone", ok, detail)

        if os.path.isfile(args.disk):
            on_disk = image_files(args.disk, os.path.join(tmp, "image"))
            drift = compare(tree_files(sysroot), on_disk)
            check(f"{os.path.basename(args.disk)}'s /usr/include and /usr/lib are build/sysroot", not drift,
                  "; ".join(drift[:4]) + (f" (+{len(drift) - 4})" if len(drift) > 4 else "")
                  + " -- `make iso` re-seeds")
        else:
            print(f"  skip {args.disk} is absent -- `make iso` builds it")

        prune_checks(tmp, check)

    if args.positive_control:
        caught = sum(1 for b in bad if not b.startswith(("a re-seed", "a file", "a directory")))
        if caught >= 2:
            print("sdk_test: POSITIVE CONTROL OK -- the tampered SDK failed "
                  f"{caught} comparisons, as it must")
            return 0
        print("sdk_test: POSITIVE CONTROL FAILED -- a tampered SDK passed")
        return 1
    print(f"sdk_test: {'FAILED' if bad else 'PASSED'}" + (f" ({len(bad)})" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
