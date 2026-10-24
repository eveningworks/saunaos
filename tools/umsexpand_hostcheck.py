#!/usr/bin/env python3
"""Check lib/umsexpand.c -- the SZDD and KWAJ expander -- three ways.

WHAT IS UNDER TEST. userland/lib/umsexpand.c, which w16's Windows
installer runs over setup disks nobody here made, compiled with the host
gcc under AddressSanitizer and UBSan beside a small driver.

What it asserts:

  - ROUND TRIPS through a second implementation: tools/mscompress.py
    writes SZDD and KWAJ methods 0-3 (method 3 in each of its four
    code-length encodings) of empty, tiny, random, repetitive and >64 KiB
    data, and the driver must give back the same bytes and name
  - REAL DISKS, when ~/CodingProjects/toy-osother/win311 is there (it is
    local only -- Microsoft's files): every file of the English and
    Finnish Windows for Workgroups 3.11 sets expands byte for byte to what
    libmspack made of it (win311/<lang>/expanded). Skipped, saying so,
    anywhere else
  - HOSTILE FILES: thousands of truncated and mutated copies of each
    written file through the expander, each in a buffer exactly its size:
    every one expands or refuses, and nothing reads outside it

POSITIVE CONTROL (--positive-control): two edits to a COPY of the source.
MATCHLEN2 gets libmspack-less 32 symbols (the round trips must go red),
and the LZSS reader stops checking that a match's two bytes are there
(the fuzz must go red under ASan).

    python3 tools/umsexpand_hostcheck.py [--positive-control] [--iters N]

Exit status is non-zero on any violation. Needs gcc with ASan/UBSan.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402
import mscompress  # noqa: E402

ROOT = hostcheck.ROOT
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
       "-fno-sanitize-recover=undefined")
W311 = os.path.expanduser(os.environ.get("TOYOS_W311", "~/CodingProjects/toy-osother/win311"))

DRIVER = r"""
#include "lib/umsexpand.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_o;
static int to_file(void *c, const uint8_t *d, size_t n) { (void)c; return fwrite(d, 1, n, g_o) != n; }
static int count(void *c, const uint8_t *d, size_t n) { (void)d; *(size_t *)c += n; return 0; }

static uint8_t *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);
    uint8_t *b = malloc(*n ? (size_t)*n : 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    static struct umsx_scratch s;
    if (argc == 5 && !strcmp(argv[1], "expand")) {
        long n;
        uint8_t *b = slurp(argv[2], &n);
        struct umsx_info info;
        char name[13] = "";
        int pr = umsx_probe(b, (size_t)n, &info);
        if (pr == 0) umsx_name(&info, argv[4], name, sizeof name);
        g_o = fopen(argv[3], "wb");
        uint32_t w = 0;
        int r = umsx_expand(b, (size_t)n, &s, to_file, 0, &w);
        fclose(g_o);
        printf("%d %u %d %d %s\n", r, w, (int)info.kind, info.method, name);
        free(b);
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "fuzz")) {
        long n;
        uint8_t *orig = slurp(argv[2], &n);
        int iters = atoi(argv[3]);
        srand((unsigned)atoi(argv[4]));
        int ok = 0, refused = 0;
        for (int i = 0; i < iters; i++) {
            size_t m = (size_t)n;
            if (i % 3 == 0 && m) m = (size_t)rand() % m;          // truncated
            uint8_t *c = malloc(m ? m : 1);                      // EXACTLY its size, for ASan
            memcpy(c, orig, m);
            int flips = i % 3 == 0 ? 0 : 1 + rand() % 8;
            for (int k = 0; k < flips && m; k++) {
                size_t at = (size_t)rand() % m;
                if (rand() & 1) c[at] ^= (uint8_t)(1u << (rand() % 8));
                else c[at] = (uint8_t)rand();
            }
            size_t got = 0;
            uint32_t w;
            if (umsx_expand(c, m, &s, count, &got, &w) == 0) ok++;
            else refused++;
            free(c);
        }
        printf("fuzz %d %d\n", ok, refused);
        free(orig);
        return 0;
    }
    fprintf(stderr, "usage: expand IN OUT DISKNAME | fuzz IN ITERS SEED\n");
    return 2;
}
"""

CONTROL = [
    ("static const int KWAJ_NSYMS[5] = { 16, 16, 32, 64, 256 };",
     "static const int KWAJ_NSYMS[5] = { 16, 32, 32, 64, 256 };"),
    ("if (end - p < 2) return 0;", "if (end - p < 0) return 0;"),
]

failures = []


def check(name, ok, detail=""):
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", name, ("   [%s]" % detail) if detail and not ok else ""))
    if not ok:
        failures.append(name)
    return ok


def run(exe, *args):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1")
    r = subprocess.run([exe, *map(str, args)], capture_output=True, env=env, timeout=600)
    return r.returncode, r.stdout.decode("latin-1"), r.stderr.decode("latin-1")


def expand(exe, tmp, blob, disk_name):
    src = os.path.join(tmp, "in.bin")
    dst = os.path.join(tmp, "out.bin")
    with open(src, "wb") as f:
        f.write(blob)
    rc, out, err = run(exe, "expand", src, dst, disk_name)
    if rc != 0:   # the driver died: ASan or UBSan said why
        lines = err.strip().splitlines()
        return None, None, next((l for l in lines if "ERROR" in l or "runtime error" in l),
                                lines[-1] if lines else "exit %d" % rc)
    fields = out.split()
    with open(dst, "rb") as f:
        data = f.read()
    return int(fields[0]), data, fields[4] if len(fields) > 4 else ""


def datasets():
    rnd = random.Random(1194)
    text = open(os.path.join(ROOT, "tools", "mscompress.py"), "rb").read()
    return [
        ("empty", b""),
        ("one byte", b"A"),
        ("random 5000", bytes(rnd.randrange(256) for _ in range(5000))),
        ("text", text),
        ("70 KiB of zeros", b"\0" * 70000),
        ("text three times, past the window", text * 3),
        ("bytes 0-255 x 300", bytes(range(256)) * 300),
    ]


def round_trips(exe, tmp):
    print("round trips through tools/mscompress.py:")
    for label, data in datasets():
        variants = [("SZDD", mscompress.szdd(data, b"L"), "FILE.TX_", "FILE.TXL")]
        for m in (0, 1, 2):
            variants.append(("KWAJ %d" % m, mscompress.kwaj(data, m, "FILE.TXT"), "FILE.TX_", "FILE.TXT"))
        for ft in (None, 1, 2, 3):
            variants.append(("KWAJ 3 lens type %s" % (ft if ft is not None else "best"),
                             mscompress.kwaj(data, 3, "FILE.TXT", force_type=ft), "FILE.TX_", "FILE.TXT"))
        variants.append(("KWAJ 2 no length", mscompress.kwaj(data, 2, None, with_length=False),
                         "FILE.TX_", "FILE.TX_"))
        bad = []
        for vlabel, blob, disk, want_name in variants:
            rc, got, name = expand(exe, tmp, blob, disk)
            if rc != 0 or got != data or name != want_name:
                bad.append("%s: rc %s, %s bytes, name %s" % (vlabel, rc, None if got is None else len(got), name))
        check("%s (%d bytes): %d encodings" % (label, len(data), len(variants)), not bad, "; ".join(bad[:3]))


def real_disks(exe, tmp):
    if not os.path.isdir(W311):
        print("real disks: SKIPPED -- %s is not here (Microsoft's files stay local)" % W311)
        return
    print("real disks (%s):" % W311)
    for lang in ("en", "fi"):
        inst, expd = os.path.join(W311, lang, "install"), os.path.join(W311, lang, "expanded")
        if not os.path.isdir(inst):
            continue
        names = {f.rstrip(" .").upper(): f for f in os.listdir(expd)}   # "WINVER. " as libmspack named it
        same = 0
        bad = []
        for f in sorted(os.listdir(inst)):
            with open(os.path.join(inst, f), "rb") as fh:
                blob = fh.read()
            if not (blob.startswith(mscompress.KWAJ_MAGIC) or blob.startswith(mscompress.SZDD_MAGIC)):
                continue    # a plain file is copied, not expanded: nothing to compare
            rc, got, name = expand(exe, tmp, blob, f)
            ref = names.get((name or "").rstrip(".").upper())
            if rc == 0 and ref and open(os.path.join(expd, ref), "rb").read() == got:
                same += 1
            else:
                bad.append("%s -> %s rc %s" % (f, name, rc))
        check("%s: %d compressed files expand as libmspack's did" % (lang, same + len(bad)),
              not bad and same > 0, "; ".join(bad[:4]))


def fuzz(exe, tmp, iters):
    print("hostile files (%d mutations each, exact-size buffers):" % iters)
    rnd = random.Random(7)
    data = bytes(rnd.randrange(256) for _ in range(600)) + open(
        os.path.join(ROOT, "tools", "mscompress.py"), "rb").read()[:6000]
    for label, blob in (("SZDD", mscompress.szdd(data)), ("KWAJ 2", mscompress.kwaj(data, 2, "A.B")),
                        ("KWAJ 3", mscompress.kwaj(data, 3, "A.B")),
                        ("KWAJ 3 type 1", mscompress.kwaj(data, 3, "A.B", force_type=1))):
        path = os.path.join(tmp, "fuzz.bin")
        with open(path, "wb") as f:
            f.write(blob)
        rc, out, err = run(exe, "fuzz", path, iters, 11)
        detail = (err.strip().splitlines() or ["exit %d" % rc])
        detail = next((l for l in detail if "ERROR" in l or "runtime error" in l), detail[-1])
        check("%s: every mutation expands or refuses, nothing out of bounds" % label,
              rc == 0 and out.startswith("fuzz"), detail)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    ap.add_argument("--iters", type=int, default=4000)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="umsexpand-") as tmp:
        drv = hostcheck.write(tmp, "umsx_host.c", DRIVER)
        src = hostcheck.stage(tmp, "userland/lib/umsexpand.c", dest="lib/umsexpand.c", edits=CONTROL,
                              apply=args.positive_control, tool="umsexpand_hostcheck")
        exe = hostcheck.compile(tmp, "umsx_host", [drv, src], flags=SAN,
                                includes=[os.path.join(ROOT, "userland")], tool="umsexpand_hostcheck")
        round_trips(exe, tmp)
        real_disks(exe, tmp)
        fuzz(exe, tmp, args.iters)
    if args.positive_control:
        print("\numsexpand_hostcheck: POSITIVE CONTROL -- %d check(s) went red%s" %
              (len(failures), "" if failures else " -- NONE: the check cannot see the break"))
        return 0 if failures else 1
    print("\numsexpand_hostcheck: %s" % ("PASS" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
