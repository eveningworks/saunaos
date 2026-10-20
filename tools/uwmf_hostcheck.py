#!/usr/bin/env python3
"""Check lib/uwmf's metafile decoder against an independent writer, and against hostile files.

WHAT IS UNDER TEST. userland/lib/uwmf.c -- the Windows metafile (MS-WMF)
record decoder w16's PlayMetaFile and OLE pictures draw through --
compiled with the host gcc beside a small driver that prints every record
it decodes.

What it asserts:

  - A METAFILE BUILT HERE, by a writer that shares no code with the
    decoder (records laid out from MS-WMF's tables, arguments last first),
    decodes to exactly the records written: each one's function, its words
    in GDI's order, colour, ROP, object slot, points, polygon counts, text,
    ExtTextOut's rectangle and advances, a DIB's length and a font's face.
    Once bare, once behind a placeable header (0x9AC6CDD7), whose bounding
    box and units per inch must read back.
  - THE OBJECT TABLE: creates take the lowest free slot, a delete frees
    it for the next create, as MS-WMF defines it.
  - HOSTILE FILES: thousands of mutated and truncated copies through
    uwmf_next, under AddressSanitizer and UBSan -- each either decodes,
    ends, or is refused, and never reads outside the bytes given.

POSITIVE CONTROL (--positive-control): two edits to a COPY of uwmf.c. The
Polygon record stops checking its point count against the record (the
fuzz must go red under ASan), and the plain-word records keep the file's
order instead of GDI's (the comparison must go red).

Needs gcc with ASan/UBSan.
"""

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/uwmf.h"

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz ? (size_t)sz : 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) exit(2);
    fclose(f); *n = (size_t)sz; return b;
}

static void dump(const uint8_t *d, size_t n) {
    struct uwmf m;
    if (uwmf_open(&m, d, n) < 0) { printf("refused\n"); return; }
    if (m.placeable) printf("placeable %d %d %d %d inch %u\n", m.bounds[0], m.bounds[1], m.bounds[2], m.bounds[3], m.inch);
    struct uwmf_rec r;
    int got;
    while ((got = uwmf_next(&m, &r)) > 0) {
        printf("rec %04x slot %d color %08x rop %08x v", r.func, r.slot, r.color, r.rop);
        for (int i = 0; i < r.nv; i++) printf(" %d", r.v[i]);
        printf(" pt");
        for (int i = 0; i < 2 * r.npt; i++) printf(" %d", r.pt[i]);
        printf(" counts");
        for (int i = 0; i < r.ncounts; i++) printf(" %u", r.counts[i]);
        printf(" text %.*s", r.ntext, r.text ? r.text : "");
        printf(" dx");
        for (int i = 0; r.dx && i < r.ntext; i++) printf(" %d", r.dx[i]);
        printf(" rect %d %d %d %d dib %u face %s\n", r.rect[0], r.rect[1], r.rect[2], r.rect[3], r.ndib, r.face);
    }
    printf("end %d\n", got);
    uwmf_close(&m);
}

// Every record of a mutated copy, read through: a missing bound shows under ASan.
static int fuzz(const uint8_t *d, size_t n, unsigned seed) {
    srand(seed);
    int decoded = 0;
    for (int k = 0; k < 4000; k++) {
        size_t len = k % 7 == 0 ? (size_t)rand() % (n + 1) : n;
        uint8_t *c = malloc(len ? len : 1);   // exactly `len`: a read past it is caught
        memcpy(c, d, len);
        int flips = 1 + rand() % 6;
        for (int f = 0; f < flips && len; f++) c[(size_t)rand() % len] = (uint8_t)rand();
        struct uwmf m;
        if (uwmf_open(&m, c, len) == 0) {
            struct uwmf_rec r;
            int steps = 0;
            while (uwmf_next(&m, &r) > 0 && steps++ < 10000) {
                volatile int sink = 0;
                for (int i = 0; i < 2 * r.npt; i++) sink += r.pt[i];
                for (int i = 0; i < r.ncounts; i++) sink += r.counts[i];
                for (int i = 0; i < r.ntext; i++) sink += r.text[i];
                for (int i = 0; r.dx && i < r.ntext; i++) sink += r.dx[i];
                for (uint32_t i = 0; i < r.ndib; i++) sink += r.dib[i];
                for (uint32_t i = 0; i < r.nwords; i++) sink += r.words[i];
                (void)sink;
                decoded++;
            }
            uwmf_close(&m);
        }
        free(c);
    }
    printf("%d records decoded from 4000 hostile copies\n", decoded);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    setvbuf(stdout, 0, _IOLBF, 0);
    size_t n;
    uint8_t *d = slurp(argv[2], &n);
    int r = 0;
    if (!strcmp(argv[1], "dump")) dump(d, n);
    else r = fuzz(d, n, (unsigned)atoi(argc > 3 ? argv[3] : "1"));
    free(d);
    return r;
}
"""

CONTROL = [
    ("NEED(1 + 2ul * np);", ""),
    ("for (unsigned i = 0; i < k; i++) r->v[i] = W(nw - 1 - i);", "for (unsigned i = 0; i < k; i++) r->v[i] = W(i);"),
]


# --- the independent writer: records from MS-WMF's tables ------------------------

def rec(func, words=b""):
    assert len(words) % 2 == 0
    return struct.pack("<IH", 3 + len(words) // 2, func) + words


def w(*vals):
    return struct.pack("<" + "h" * len(vals), *vals)


def colorref(r, g, b):
    return struct.pack("<I", r | g << 8 | b << 16)


def build():
    """(the metafile's bytes, the lines the decoder must print for it)"""
    dib = struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 24, 0, 16, 0, 0, 0, 0) + b"\x00\x00\xff\x00\xff\x00\x00\x00" * 2
    face = b"Times New Roman\x00"
    recs, want = [], []

    def add(func, words, line):
        recs.append(rec(func, words))
        want.append(line)

    def line(func, slot=-1, color=0, rop=0, v=(), pt=(), counts=(), text=b"", dx=(), rect=(0, 0, 0, 0), dib=0,
             face=""):
        return (f"rec {func:04x} slot {slot} color {color:08x} rop {rop:08x} v" + "".join(f" {x}" for x in v)
                + " pt" + "".join(f" {x}" for x in pt) + " counts" + "".join(f" {x}" for x in counts)
                + f" text {text.decode('latin1')} dx" + "".join(f" {x}" for x in dx)
                + f" rect {rect[0]} {rect[1]} {rect[2]} {rect[3]} dib {dib} face {face}")

    add(0x020C, w(300, 400), line(0x020C, v=(400, 300)))                       # SetWindowExt(400, 300)
    add(0x020B, w(-7, 5), line(0x020B, v=(5, -7)))                             # SetWindowOrg(5, -7)
    add(0x0102, w(1, 0), line(0x0102, v=(1,)))                                 # SetBkMode, a reserved word after
    add(0x0201, colorref(1, 2, 3), line(0x0201, color=0x030201))               # SetBkColor
    add(0x02FC, w(0) + colorref(255, 0, 0) + w(0), line(0x02FC, slot=0, color=0x0000FF, v=(0, 0)))
    add(0x02FA, w(0, 3, 0) + colorref(0, 0, 255), line(0x02FA, slot=1, color=0xFF0000, v=(0, 3)))
    add(0x012D, w(0), line(0x012D, slot=0))                                    # SelectObject(0)
    add(0x041B, w(40, 30, 20, 10), line(0x041B, v=(10, 20, 30, 40)))           # Rectangle(10, 20, 30, 40)
    add(0x061D, struct.pack("<I", 0x00F00021) + w(8, 9, 2, 1),
        line(0x061D, rop=0x00F00021, v=(1, 2, 9, 8)))                          # PatBlt(1, 2, 9, 8, PATCOPY)
    add(0x0324, w(3, 0, 0, 10, 0, 0, 10), line(0x0324, pt=(0, 0, 10, 0, 0, 10)))
    add(0x0538, w(2, 3, 4) + w(0, 0, 5, 0, 0, 5) + w(10, 10, 20, 10, 20, 20, 10, 20),
        line(0x0538, counts=(3, 4), pt=(0, 0, 5, 0, 0, 5, 10, 10, 20, 10, 20, 20, 10, 20)))
    add(0x0521, w(3) + b"abc\x00" + w(50, 60), line(0x0521, text=b"abc", v=(60, 50)))   # TextOut(60, 50, "abc")
    add(0x0A32, w(70, 80, 2, 4) + w(1, 2, 3, 4) + b"xy" + w(7, 8),
        line(0x0A32, text=b"xy", v=(80, 70, 4), dx=(7, 8), rect=(1, 2, 3, 4)))  # ExtTextOut, ETO_CLIPPED
    add(0x0B41, struct.pack("<I", 0x00CC0020) + w(2, 2, 0, 0, 20, 20, 5, 6) + dib,
        line(0x0B41, rop=0x00CC0020, v=(6, 5, 20, 20, 0, 0, 2, 2), dib=len(dib)))   # DibStretchBlt
    add(0x01F0, w(0), line(0x01F0, slot=0))                                    # DeleteObject(0): slot 0 free again
    add(0x02FB, w(-16, 0, 0, 0, 700) + bytes([1, 0, 0, 0, 0, 0, 0, 0x12]) + face,
        line(0x02FB, slot=0, v=(-16, 0, 0, 0, 700, 1, 0, 0, 0, 0, 0, 0, 18), face="Times New Roman"))
    add(0x0999, w(1, 2), line(0x0999, v=(2, 1)))                               # a record no table names
    body = b"".join(recs) + rec(0)
    want.append("end 0")
    words = (18 + len(body)) // 2
    header = struct.pack("<HHHIHIH", 1, 9, 0x300, words, 3, 20, 0)
    return header + body, want


def run(exe, path, *args):
    return subprocess.run([exe, *args[:1], path, *args[1:]], capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break two checks in a copy of uwmf.c; the check must go RED")
    args = ap.parse_args()
    fails = []
    data, want = build()
    with tempfile.TemporaryDirectory() as tmp:
        src = hostcheck.stage(tmp, "userland/lib/uwmf.c", dest="lib/uwmf.c", edits=CONTROL,
                              apply=args.positive_control, tool="uwmf_hostcheck")
        hostcheck.stage(tmp, "userland/lib/uwmf.h", dest="lib/uwmf.h")
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all"]
        exe = hostcheck.compile(tmp, "uwmf_host", [src, drv], flags=flags, includes=[tmp], tool="uwmf_hostcheck")

        for name, blob, extra in (("bare", data, []),
                                  ("placeable", struct.pack("<IHhhhhHIH", 0x9AC6CDD7, 0, -1, -2, 400, 300, 1440, 0, 0)
                                   + data, ["placeable -1 -2 400 300 inch 1440"])):
            path = os.path.join(tmp, name + ".wmf")
            with open(path, "wb") as fh:
                fh.write(blob)
            got = run(exe, path, "dump")
            lines = got.stdout.splitlines()
            expect = extra + want
            if got.returncode or lines != expect:
                fails.append(f"{name}: the decoded records differ from the ones written")
                if not args.positive_control:
                    for a, b in zip(expect, lines + [""] * len(expect)):
                        if a != b:
                            print(f"    want {a}\n    got  {b}")
                            break
            else:
                print(f"  ok  {name}: {len(want) - 1} records decode as written")
            for seed in (1, 2, 3):
                fz = run(exe, path, "fuzz", str(seed))
                if fz.returncode:
                    fails.append(f"{name}: the decoder faulted on a hostile copy")
                    why = [ln for ln in fz.stderr.splitlines() if "ERROR:" in ln or "runtime error" in ln]
                    print(f"  FAIL {name} seed {seed}: " + (why[0].strip() if why else f"exit {fz.returncode}"))
                    break
            else:
                print(f"  ok  {name}: {fz.stdout.strip()}, no fault (3 seeds)")
        # a random file of the right header and nothing else must end or be refused, not loop
        junk = struct.pack("<HHHIHIH", 1, 9, 0x300, 9, 0, 0, 0) + bytes(random.Random(7).randrange(256) for _ in range(64))
        jp = os.path.join(tmp, "junk.wmf")
        with open(jp, "wb") as fh:
            fh.write(junk)
        if run(exe, jp, "dump").returncode:
            fails.append("junk: the decoder faulted")

    if args.positive_control:
        red = ("differ" in " ".join(fails), "faulted" in " ".join(fails))
        print(f"uwmf_hostcheck: positive control: comparison {'RED' if red[0] else 'green'}, "
              f"fuzz {'RED' if red[1] else 'green'}")
        return 0 if all(red) else 1
    for f in fails:
        print("  FAIL " + f)
    print(f"uwmf_hostcheck: {'FAILED' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
