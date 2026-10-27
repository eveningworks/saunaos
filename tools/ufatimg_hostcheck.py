#!/usr/bin/env python3
"""Check lib/ufatimg.c -- FAT12/FAT16 disk images read from memory.

WHAT IS UNDER TEST. userland/lib/ufatimg.c, which w16's Windows installer
uses to read setup floppies as image files, compiled with the host gcc
under AddressSanitizer and UBSan beside a small driver.

What it asserts:

  - against MTOOLS, a second reader: images it formats -- a 1.44 MB FAT12
    floppy, a 1.68 MB DMF one (21 sectors a track, as Microsoft's later
    disks) and a FAT16 volume -- holding files of 0, 1, 511, 512, 513 and
    300,000 bytes, a subdirectory two deep, a deleted file and a long-named
    one: the label, every listing (8.3 names, sizes) and every file's bytes
  - REAL DISKS, when ~/CodingProjects/saunaos-other/win311/en/disks is there
    (local only -- Microsoft's files): each of the eight Windows for
    Workgroups 3.11 floppies lists exactly the files of win311/en/install,
    byte for byte, under its label
  - HOSTILE IMAGES: thousands of truncated and mutated copies (boot
    sector, FATs, directories), each in a buffer exactly its size: every
    listing and read finishes or refuses, and nothing reads outside it

POSITIVE CONTROL (--positive-control): two edits to a COPY of the source.
FAT12's odd and even entries swap nibbles (contents go wrong), and a
cluster stops being checked against the end of the image (the fuzz must
go red under ASan).

    python3 tools/ufatimg_hostcheck.py [--positive-control] [--iters N]

Exit status is non-zero on any violation. Needs gcc with ASan/UBSan and mtools.
"""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
       "-fno-sanitize-recover=undefined")
DISKS = os.path.expanduser(os.environ.get("SAUNAOS_W311", "~/CodingProjects/saunaos-other/win311")) + "/en"

DRIVER = r"""
#include "lib/ufatimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int print_ent(void *c, const struct ufatimg_ent *e) {
    (void)c;
    printf("%s %c %u\n", e->name, e->attr & UFATIMG_DIR ? 'd' : 'f', e->size);
    return 0;
}
static FILE *g_o;
static int to_file(void *c, const uint8_t *d, size_t n) { (void)c; return fwrite(d, 1, n, g_o) != n; }
static int count(void *c, const uint8_t *d, size_t n) { (void)d; *(size_t *)c += n; return 0; }

// "A/B/C.TXT" from the root; 0, or the error.
static int walk(const struct ufatimg *v, const char *path, struct ufatimg_ent *out) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", path);
    uint32_t dir = 0;
    for (char *s = strtok(buf, "/"); s; s = strtok(0, "/")) {
        int r = ufatimg_find(v, dir, s, out);
        if (r) return r;
        dir = out->cluster;
    }
    return 0;
}

struct walkall { const struct ufatimg *v; size_t bytes; int depth; };
static int visit(void *c, const struct ufatimg_ent *e) {
    struct walkall *w = c;
    if (e->attr & UFATIMG_DIR) {
        if (w->depth < 8 && e->cluster) { w->depth++; ufatimg_list(w->v, e->cluster, visit, w); w->depth--; }
    } else {
        ufatimg_read(w->v, e, count, &w->bytes);
    }
    return 0;
}

int main(int argc, char **argv) {
    long n;
    if (argc < 3) return 2;
    uint8_t *img = slurp(argv[2], &n);
    struct ufatimg v;
    if (!strcmp(argv[1], "fuzz") && argc == 5) {
        int iters = atoi(argv[3]), opened = 0;
        srand((unsigned)atoi(argv[4]));
        long hot = n < 40000 ? n : 40000;               // boot sector, FATs, root: where the numbers are
        for (int i = 0; i < iters; i++) {
            size_t m = (size_t)n;
            if (i % 4 == 0) m = (size_t)rand() % (size_t)n;
            uint8_t *c = malloc(m ? m : 1);              // EXACTLY its size, for ASan
            memcpy(c, img, m);
            int flips = i % 4 == 0 ? 0 : 1 + rand() % 12;
            for (int k = 0; k < flips && m; k++) {
                size_t at = (size_t)rand() % ((size_t)hot < m ? (size_t)hot : m);
                c[at] = (rand() & 1) ? (uint8_t)(c[at] ^ (1u << (rand() % 8))) : (uint8_t)rand();
            }
            if (ufatimg_open(&v, c, m) == 0) {
                opened++;
                struct walkall w = { &v, 0, 0 };
                ufatimg_list(&v, 0, visit, &w);
            }
            free(c);
        }
        printf("fuzz %d\n", opened);
        free(img);
        return 0;
    }
    int r = ufatimg_open(&v, img, (size_t)n);
    if (r) { printf("open %d\n", r); free(img); return 0; }
    if (!strcmp(argv[1], "label")) {
        printf("label %s\n", v.label);
    } else if (!strcmp(argv[1], "list") && argc == 4) {
        struct ufatimg_ent e = { "", UFATIMG_DIR, 0, 0 };
        if (strcmp(argv[3], "/") && (r = walk(&v, argv[3], &e))) printf("walk %d\n", r);
        else printf("count %d\n", ufatimg_list(&v, e.cluster, print_ent, 0));
    } else if (!strcmp(argv[1], "cat") && argc == 5) {
        struct ufatimg_ent e;
        if ((r = walk(&v, argv[3], &e))) printf("walk %d\n", r);
        else { g_o = fopen(argv[4], "wb"); printf("read %d\n", ufatimg_read(&v, &e, to_file, 0)); fclose(g_o); }
    }
    free(img);
    return 0;
}
"""

CONTROL = [
    ("x = c & 1 ? x >> 4 : x & 0xFFF;", "x = c & 1 ? x & 0xFFF : x >> 4;"),
    ("if (at + *len > v->n) return 0;", "if (at > v->n) return 0;"),
]

failures = []


def check(name, ok, detail=""):
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", name, ("   [%s]" % detail) if detail and not ok else ""))
    if not ok:
        failures.append(name)
    return ok


def run(exe, *args):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1")
    r = subprocess.run([exe, *map(str, args)], capture_output=True, env=env, timeout=900)
    return r.returncode, r.stdout.decode("latin-1"), r.stderr.decode("latin-1")


def mt(*args):
    r = subprocess.run(list(args), capture_output=True, text=True,
                       env=dict(os.environ, MTOOLS_SKIP_CHECK="1", MTOOLS_NO_VFAT="0"))
    if r.returncode != 0:
        sys.exit("ufatimg_hostcheck: %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def build_images(tmp):
    rnd = random.Random(31)
    files = {"EMPTY.TXT": b"", "ONE.BIN": b"x", "S511.BIN": bytes(rnd.randrange(256) for _ in range(511)),
             "S512.BIN": bytes(rnd.randrange(256) for _ in range(512)),
             "S513.BIN": bytes(rnd.randrange(256) for _ in range(513)),
             "BIG.DAT": bytes(rnd.randrange(256) for _ in range(300000))}
    sub = {"SUB/INNER.TXT": b"inside\r\n" * 100, "SUB/DEEP/LAST.TXT": b"two deep"}
    long_name = ("A long file name.txt", b"long")
    images = []
    for label, fmt in (("FLOPPY144", ["-f", "1440"]), ("DMF168", ["-t", "80", "-h", "2", "-n", "21"]),
                       ("FAT16VOL", ["-t", "160", "-h", "4", "-n", "63"])):
        img = os.path.join(tmp, label.lower() + ".img")
        size = {"FLOPPY144": 1474560, "DMF168": 80 * 2 * 21 * 512, "FAT16VOL": 160 * 4 * 63 * 512}[label]
        with open(img, "wb") as f:
            f.truncate(size)
        mt("mformat", "-i", img, *fmt, "-v", label, "::")
        for name, data in list(files.items()) + [("GONE.TXT", b"deleted")]:
            p = os.path.join(tmp, "f.bin")
            with open(p, "wb") as f:
                f.write(data)
            mt("mcopy", "-i", img, p, "::/" + name)
        mt("mdel", "-i", img, "::/GONE.TXT")
        mt("mmd", "-i", img, "::/SUB")
        mt("mmd", "-i", img, "::/SUB/DEEP")
        for name, data in sub.items():
            p = os.path.join(tmp, "f.bin")
            with open(p, "wb") as f:
                f.write(data)
            mt("mcopy", "-i", img, p, "::/" + name)
        p = os.path.join(tmp, "f.bin")
        with open(p, "wb") as f:
            f.write(long_name[1])
        mt("mcopy", "-i", img, p, "::/" + long_name[0])
        images.append((label, img, dict(files, **sub)))
    return images


def mtools_listing(img, path):
    """mtools' own reading of a directory: {SHORT.NAME: (kind, size)}. Its
    sizes group thousands with spaces ("300 000"), so the date anchors the line."""
    out = mt("mdir", "-i", img, "-a", "::" + path)
    got = {}
    for line in out.splitlines():
        m = re.match(r"^(\S+)(?: +(\S{1,3}))? +(<DIR>|\d[\d ]*) +\d{4}-\d\d-\d\d", line)
        if not m or m.group(1) in (".", ".."):
            continue
        short = m.group(1) + ("." + m.group(2) if m.group(2) else "")
        got[short.upper()] = ("d", 0) if m.group(3) == "<DIR>" else ("f", int(m.group(3).replace(" ", "")))
    return got


def ours(exe, img, path):
    rc, out, err = run(exe, "list", img, path)
    if rc != 0:
        return None, err.strip().splitlines()[-1:] or ["exit %d" % rc]
    got = {}
    for line in out.splitlines():
        p = line.split(" ")
        if len(p) == 3:
            got[p[0]] = (p[1], int(p[2]) if p[1] == "f" else 0)
    return got, out


def against_mtools(exe, tmp, images):
    print("against mtools:")
    for label, img, files in images:
        rc, out, _ = run(exe, "label", img)
        check("%s: the label" % label, out.strip() == "label " + label, out.strip())
        for path in ("/", "/SUB", "/SUB/DEEP"):
            want = mtools_listing(img, path)
            got, raw = ours(exe, img, path)
            check("%s: %s lists as mtools reads it (%d entries)" % (label, path, len(want)),
                  got == want, "ours %s vs mtools %s" % (got, want))
        bad = []
        for name, data in files.items():
            dst = os.path.join(tmp, "cat.bin")
            rc, out, err = run(exe, "cat", img, name, dst)
            if rc != 0 or out.strip() != "read 0" or open(dst, "rb").read() != data:
                bad.append("%s: %s" % (name, out.strip() or err.strip()[-120:]))
        check("%s: every file's bytes (%d files)" % (label, len(files)), not bad, "; ".join(bad[:3]))


def real_disks(exe, tmp):
    disks, inst = os.path.join(DISKS, "disks"), os.path.join(DISKS, "install")
    if not os.path.isdir(disks):
        print("real disks: SKIPPED -- %s is not here (Microsoft's files stay local)" % disks)
        return
    print("real disks (%s):" % disks)
    want = {f.upper(): f for f in os.listdir(inst)}
    seen = 0
    for d in sorted(os.listdir(disks)):
        img = os.path.join(disks, d)
        rc, out, _ = run(exe, "label", img)
        got, _ = ours(exe, img, "/")
        bad = []
        for name, (kind, size) in (got or {}).items():
            if kind != "f":
                continue
            ref = want.get(name)
            dst = os.path.join(tmp, "cat.bin")
            run(exe, "cat", img, name, dst)
            if not ref or open(os.path.join(inst, ref), "rb").read() != open(dst, "rb").read():
                bad.append(name)
        seen += len(got or {})
        check("%s (%s): %d files, each as the install set has it" % (d, out.strip(), len(got or {})),
              got is not None and not bad and got, "; ".join(bad[:4]))
    check("the eight disks hold the install set's %d files" % len(want), seen == len(want),
          "%d listed" % seen)


def fuzz(exe, images, iters):
    print("hostile images (%d mutations each, exact-size buffers):" % iters)
    for label, img, _ in images:
        rc, out, err = run(exe, "fuzz", img, iters, 5)
        lines = err.strip().splitlines()
        detail = next((l for l in lines if "ERROR" in l or "runtime error" in l), lines[-1] if lines else "")
        check("%s: every mutation lists and reads, or refuses" % label, rc == 0 and out.startswith("fuzz"), detail)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    ap.add_argument("--iters", type=int, default=3000)
    args = ap.parse_args()
    if not shutil.which("mformat"):
        sys.exit("ufatimg_hostcheck: needs mtools (mformat, mcopy, mdir)")
    with tempfile.TemporaryDirectory(prefix="ufatimg-") as tmp:
        drv = hostcheck.write(tmp, "ufat_host.c", DRIVER)
        src = hostcheck.stage(tmp, "userland/lib/ufatimg.c", dest="lib/ufatimg.c", edits=CONTROL,
                              apply=args.positive_control, tool="ufatimg_hostcheck")
        exe = hostcheck.compile(tmp, "ufat_host", [drv, src], flags=SAN,
                                includes=[os.path.join(ROOT, "userland")], tool="ufatimg_hostcheck")
        images = build_images(tmp)
        against_mtools(exe, tmp, images)
        real_disks(exe, tmp)
        fuzz(exe, images, args.iters)
    if args.positive_control:
        print("\nufatimg_hostcheck: POSITIVE CONTROL -- %d check(s) went red%s" %
              (len(failures), "" if failures else " -- NONE: the check cannot see the break"))
        return 0 if failures else 1
    print("\nufatimg_hostcheck: %s" % ("PASS" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
