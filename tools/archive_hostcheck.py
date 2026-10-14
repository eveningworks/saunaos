#!/usr/bin/env python3
"""Check lib/uzip and lib/utar against Python's zipfile, tarfile and gzip, on the HOST.

BOTH DIRECTIONS, against an implementation that shares no code with
userland/lib/ -- the uimg_codec_hostcheck.py rule. An archive writer
checked only by this repo's own reader passes whenever the two share a
mistake (a CRC over the wrong bytes, a tar checksum summed with the
field in, a central directory offset one header off), and such an
archive opens here and nowhere else.

  WRITE: the C writers make .zip (store and every deflate level), .tar
         and .tar.gz of a tree with nested folders, an empty file, a
         binary file, a name over 100 bytes (a ustar prefix) and one
         that only a GNU long-name member can hold; zipfile and tarfile
         must read every member back byte for byte, zipfile's testzip()
         must find no bad CRC, and gzip must inflate the stream whole.
  READ:  Python writes .zip (stored and deflated, with folder records),
         .tar in GNU and in PAX format (long names both ways), .tar.gz
         and a lone .gz; the C readers must list every member at its
         size and extract each one byte for byte.

    python3 tools/archive_hostcheck.py
    python3 tools/archive_hostcheck.py --positive-control

The positive control breaks the tar header checksum and the zip CRC and
expects both writer families to go red. Needs gcc.
"""
import argparse
import gzip
import io
import os
import random
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from harness import Results  # noqa: E402

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/uzip.h"
#include "lib/utar.h"

static int list_zip(void *c, const char *n, unsigned long s, unsigned long p, unsigned d, unsigned t) {
    (void)c; (void)p; (void)d; (void)t; printf("%lu\t%s\n", s, n); return 0;
}
static int list_tar(void *c, const char *n, unsigned long long s, int64_t m, int dir) {
    (void)c; (void)m; (void)dir; printf("%llu\t%s\n", s, n); return 0;
}
int main(int argc, char **argv) {
    char err[256] = "";
    int rc = 0;
    if (argc >= 5 && !strcmp(argv[1], "zip")) {           // zip OUT LEVEL (f|d) NAME SRC ...
        struct uzip_writer w;
        if ((rc = uzip_create_begin(&w, argv[2], err, sizeof err))) goto out;
        int level = atoi(argv[3]);
        for (int i = 4; i + 2 < argc + 1 && i < argc; i += 3) {
            if (argv[i][0] == 'd') rc = uzip_add_dir(&w, argv[i + 1], err, sizeof err);
            else rc = uzip_add_file(&w, argv[i + 1], argv[i + 2], level, 0, 0, err, sizeof err);
            if (rc) { uzip_create_abort(&w); goto out; }
        }
        rc = uzip_create_end(&w, err, sizeof err);
    } else if (argc >= 5 && !strcmp(argv[1], "tar")) {    // tar OUT GZ LEVEL (f|d) NAME SRC ...
        struct utar_writer w;
        if ((rc = utar_create_begin(&w, argv[2], atoi(argv[3]), atoi(argv[4]), err, sizeof err))) goto out;
        for (int i = 5; i < argc; i += 3) {
            if (argv[i][0] == 'd') rc = utar_add_dir(&w, argv[i + 1], err, sizeof err);
            else rc = utar_add_file(&w, argv[i + 1], argv[i + 2], 0, 0, err, sizeof err);
            if (rc) { utar_create_abort(&w); goto out; }
        }
        rc = utar_create_end(&w, err, sizeof err);
    } else if (argc == 3 && !strcmp(argv[1], "lzip")) {
        rc = uzip_list(argv[2], list_zip, 0, err, sizeof err);
    } else if (argc == 3 && !strcmp(argv[1], "ltar")) {
        rc = utar_list(argv[2], list_tar, 0, err, sizeof err);
    } else if (argc == 5 && !strcmp(argv[1], "xzip")) {
        rc = uzip_extract(argv[2], argv[3], argv[4], 0, 0, err, sizeof err);
    } else if (argc == 5 && !strcmp(argv[1], "xtar")) {
        rc = utar_extract(argv[2], argv[3], argv[4], 0, 0, err, sizeof err);
    } else {
        fprintf(stderr, "usage\n");
        return 2;
    }
out:
    if (rc) { fprintf(stderr, "rc %d: %s\n", rc, err); return 1; }
    return 0;
}
'''

# The headers the libraries reach for outside userland/lib: the CRC,
# sys_stat() over the host's stat(), and the calendar over timegm().
SYS_SHIM = r'''
#ifndef SHIM_RT_SYS_H
#define SHIM_RT_SYS_H
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include "rtctime.h"
struct sys_stat { uint64_t size; uint32_t is_dir; struct rtc_time modified; };
static inline int sys_stat(const char *p, struct sys_stat *o) {
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    struct tm tm;
    gmtime_r(&st.st_mtime, &tm);
    o->size = (uint64_t)st.st_size;
    o->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    o->modified.year = (uint16_t)(tm.tm_year + 1900);
    o->modified.month = (uint8_t)(tm.tm_mon + 1);
    o->modified.day = (uint8_t)tm.tm_mday;
    o->modified.hour = (uint8_t)tm.tm_hour;
    o->modified.minute = (uint8_t)tm.tm_min;
    o->modified.second = (uint8_t)tm.tm_sec;
    return 0;
}
#endif
'''
CAL_SHIM = r'''
#include <stdint.h>
#include <time.h>
#include "rtctime.h"
static inline uint64_t cal_rtc_to_epoch(const struct rtc_time *t) {
    struct tm tm = { 0 };
    tm.tm_year = t->year - 1900; tm.tm_mon = t->month - 1; tm.tm_mday = t->day;
    tm.tm_hour = t->hour; tm.tm_min = t->minute; tm.tm_sec = t->second;
    return (uint64_t)timegm(&tm);
}
'''

SABOTAGE = [
    ("userland/lib/utar.c", "    for (int i = 0; i < BLOCK; i++) sum += h[i];\n    oct(h + 148, 7, sum);\n    h[155] = ' ';\n    return put(w, h, BLOCK);",
     "    for (int i = 0; i < BLOCK; i++) sum += h[i];\n    oct(h + 148, 7, sum + 1);\n    h[155] = ' ';\n    return put(w, h, BLOCK);"),
    ("userland/lib/uzip.c", "    e->crc = KCRC32_FINAL(kcrc32_update(KCRC32_INIT, raw, n));",
     "    e->crc = kcrc32_update(KCRC32_INIT, raw, n);"),
]


def build(tmp, sabotage):
    shim = os.path.join(tmp, "shim")
    os.makedirs(os.path.join(shim, "rt"), exist_ok=True)
    with open(os.path.join(shim, "kcrc.h"), "w") as f:
        f.write('#include "%s"\n' % os.path.join(ROOT, "kernel", "include", "api", "kcrc.h"))
    with open(os.path.join(shim, "rt", "sys.h"), "w") as f:
        f.write(SYS_SHIM)
    with open(os.path.join(shim, "caltime.h"), "w") as f:
        f.write(CAL_SHIM)
    srcs = {p: os.path.join(ROOT, p) for p in
            ("userland/lib/uzip.c", "userland/lib/utar.c", "userland/lib/uinflate.c")}
    if sabotage:
        for path, old, new in SABOTAGE:
            text = open(os.path.join(ROOT, path)).read()
            if text.count(old) != 1:
                sys.exit(f"positive control: {path} no longer has the line it breaks")
            broken = os.path.join(tmp, os.path.basename(path))
            with open(broken, "w") as f:
                f.write(text.replace(old, new))
            srcs[path] = broken
    drv = os.path.join(tmp, "driver.c")
    with open(drv, "w") as f:
        f.write(DRIVER)
    exe = os.path.join(tmp, "arc")
    cmd = ["gcc", "-O1", "-Wall", "-Wno-unused-parameter", "-o", exe, drv, *srcs.values(),
           os.path.join(ROOT, "kernel", "lib", "kcrc.c"),
           "-I" + shim, "-I" + os.path.join(ROOT, "userland"),
           "-idirafter", os.path.join(ROOT, "kernel", "include", "abi")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("build failed:\n" + r.stderr)
    return exe


def make_tree(base):
    """The members every WRITE check archives: name -> bytes (None = folder)."""
    rnd = random.Random(1234)
    long_prefixed = "deep/" + "p" * 60 + "/" + "q" * 70 + ".txt"        # > 100, splits at a '/'
    long_gnu = "n" * 180 + ".bin"                                          # one component > 100
    files = {
        "docs/": None,
        "docs/notes.txt": ("toy-os archive check\n" * 400).encode(),
        "docs/empty.txt": b"",
        "random.bin": bytes(rnd.getrandbits(8) for _ in range(70000)),
        # Real prose, where the levels can differ at all: the random file
        # is stored at every level and the repeated one is trivial.
        "docs/drivers.md": open(os.path.join(ROOT, "docs", "decisions", "drivers.md"), "rb").read(),
        "deep/": None,
        "deep/" + "p" * 60 + "/": None,
        long_prefixed: b"prefix + name\n",
        long_gnu: bytes(range(256)) * 9,
    }
    for name, data in files.items():
        if data is None:
            os.makedirs(os.path.join(base, name), exist_ok=True)
        else:
            path = os.path.join(base, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
    return files


def args_for(files, base):
    out = []
    for name, data in files.items():
        out += (["d", name, "-"] if data is None else ["f", name, os.path.join(base, name)])
    return out


def run(exe, *a):
    return subprocess.run([exe, *map(str, a)], capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break the tar checksum and the zip CRC; the writers must go red")
    args = ap.parse_args()
    res = Results()
    tmp = tempfile.mkdtemp(prefix="archive_hostcheck.")
    try:
        exe = build(tmp, args.positive_control)
        tree = os.path.join(tmp, "tree")
        files = make_tree(tree)
        wanted = {n: d for n, d in files.items() if d is not None}

        # --- WRITE: ours, read by Python ---------------------------------
        sizes = {}
        for level in (0, 4, 32, 512):
            out = os.path.join(tmp, f"w{level}.zip")
            r = run(exe, "zip", out, level, *args_for(files, tree))
            ok = r.returncode == 0
            detail = r.stderr.strip()
            if ok:
                try:
                    with zipfile.ZipFile(out) as z:
                        bad = z.testzip()
                        got = {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}
                        dirs = {i.filename for i in z.infolist() if i.is_dir()}
                    ok = bad is None and got == wanted and {"docs/", "deep/"} <= dirs
                    detail = f"testzip {bad!r}, {len(got)} files, dirs {sorted(dirs)}"
                    sizes[level] = os.path.getsize(out)
                except Exception as e:      # noqa: BLE001 -- the reader's verdict, whatever it is
                    ok, detail = False, f"zipfile: {e}"
            res.check(f"zipfile reads our .zip at level {level} byte for byte", ok, detail)
        if len(sizes) == 4:
            res.check("...and the levels differ: store > fast > normal >= best",
                      sizes[0] > sizes[4] > sizes[32] >= sizes[512], f"{sizes}")

        for gz, ext in ((0, "tar"), (1, "tar.gz")):
            out = os.path.join(tmp, f"w.{ext}")
            r = run(exe, "tar", out, gz, 32, *args_for(files, tree))
            ok = r.returncode == 0
            detail = r.stderr.strip()
            if ok:
                try:
                    if gz:
                        gzip.decompress(open(out, "rb").read())   # the stream whole, CRC and size
                    with tarfile.open(out) as t:
                        got = {m.name: t.extractfile(m).read() for m in t.getmembers() if m.isfile()}
                        dirs = {m.name for m in t.getmembers() if m.isdir()}
                    ok = got == wanted and {"docs", "deep"} <= dirs
                    detail = f"{len(got)} files, missing {sorted(set(wanted) - set(got))[:2]}"
                except Exception as e:      # noqa: BLE001
                    ok, detail = False, f"tarfile/gzip: {e}"
            res.check(f"tarfile reads our .{ext} byte for byte, long names included", ok, detail)

        # --- READ: Python's, read by ours --------------------------------
        fixtures = {}
        zp = os.path.join(tmp, "py.zip")
        with zipfile.ZipFile(zp, "w") as z:
            z.writestr("folder/", b"")
            z.writestr(zipfile.ZipInfo("folder/stored.txt"), b"stored member\n" * 50)
            z.writestr("folder/deflated.bin", wanted["random.bin"], compress_type=zipfile.ZIP_DEFLATED)
        fixtures["zip"] = (zp, {"folder/stored.txt": b"stored member\n" * 50,
                                "folder/deflated.bin": wanted["random.bin"]})
        for fmt, label in ((tarfile.GNU_FORMAT, "gnu"), (tarfile.PAX_FORMAT, "pax")):
            for comp in ("", "gz"):
                path = os.path.join(tmp, f"py-{label}.tar" + (".gz" if comp else ""))
                with tarfile.open(path, "w:" + comp, format=fmt) as t:
                    for name, data in wanted.items():
                        info = tarfile.TarInfo(name)
                        info.size = len(data)
                        t.addfile(info, io.BytesIO(data))
                fixtures[f"{label} tar{'.gz' if comp else ''}"] = (path, wanted)
        gp = os.path.join(tmp, "notes.txt.gz")
        with open(gp, "wb") as f:
            f.write(gzip.compress(wanted["docs/notes.txt"]))
        fixtures["lone .gz"] = (gp, {"notes.txt": wanted["docs/notes.txt"]})

        for label, (path, members) in fixtures.items():
            lister = "lzip" if label == "zip" else "ltar"
            r = run(exe, lister, path)
            listed = {}
            for line in r.stdout.splitlines():
                size, _, name = line.partition("\t")
                if not name.endswith("/"):
                    listed[name] = int(size)
            want_sizes = {n: len(d) for n, d in members.items()}
            res.check(f"our reader lists Python's {label}: every member at its size",
                      r.returncode == 0 and listed == want_sizes,
                      r.stderr.strip() or f"missing {sorted(set(want_sizes) - set(listed))[:2]}")
            bad = []
            for name, data in members.items():
                out = os.path.join(tmp, "x.out")
                x = run(exe, "xzip" if label == "zip" else "xtar", path, name, out)
                if x.returncode or open(out, "rb").read() != data:
                    bad.append(name if not x.returncode else f"{name}: {x.stderr.strip()}")
            res.check("...and extracts each byte for byte", not bad, f"{bad[:2]}")

        # A damaged tar is refused, not walked into garbage.
        broken = os.path.join(tmp, "broken.tar")
        data = bytearray(open(fixtures["gnu tar"][0], "rb").read())
        data[148] ^= 0x01   # the first header's checksum
        open(broken, "wb").write(bytes(data))
        r = run(exe, "ltar", broken)
        res.check("a tar whose header checksum is wrong is refused", r.returncode != 0,
                  r.stderr.strip())
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return res.finish("archive_hostcheck")


if __name__ == "__main__":
    sys.exit(main())
