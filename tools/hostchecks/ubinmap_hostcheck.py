#!/usr/bin/env python3
"""Check lib/ubinmap (the Hex Viewer's structure decoders) and lib/ubyteval (its inspector) on the HOST.

The decoders and the maths compile with the host gcc under ASan and UBSan beside a small driver, and
this script builds the inputs itself, so every expected node is KNOWN:

  KNOWN FILES: an ELF64 (little-endian, x86-64: two program headers, an interpreter, a GNU note,
    three sections named from .shstrtab), an ELF32 big-endian PowerPC header, a PWAD of three lumps,
    a TrueType offset table, an MBR with two partitions, a GPT disk (named entries, UTF-16), FAT16
    and FAT32 boot sectors, a QOI and a BMP -- each checked for its format line and exact nodes
    (label, value, offset, length), and the tinted runs of the ELF for disjointness and winners.
  HOSTILE FILES: counts of 2^31 and 2^32, tables past the end, offsets that wrap, a negative WAD
    header, an entry size of 0, a WAD of 100000 lumps (the node cap must hold and say so) -- and
    every prefix of every known file plus thousands of random byte flips. The driver's reader FAILS
    THE RUN if the decoder ever asks it for a byte past the end (it must have refused first), and
    every node must lie inside the file.
  THE INSPECTOR: each row of known bytes in both byte orders (signed/unsigned, the shortest float
    text, NaN and infinity, UTF-8 characters and the four ways a sequence is not one), values that
    need more bytes than remain, "As an offset", and the typed offsets, hex patterns and copy texts.

    python3 tools/hostchecks/ubinmap_hostcheck.py
    python3 tools/hostchecks/ubinmap_hostcheck.py --positive-control

**A CLEAN RUN PROVES NOTHING UNTIL --positive-control HAS GONE RED**: it patches copies of the
sources (a bounds check that always agrees, a clip removed, a field read from the wrong offset, the
tint winner inverted, a sign lost) and each must be caught by the half of the check built for it.
Needs gcc. Run by preflight.sh.
"""

# The Hex Viewer's structure decoders read any file at all. Known ELF, WAD,
# font, MBR, GPT, FAT, QOI and BMP inputs with exact nodes, the inspector's
# values, then hostile and truncated files under ASan + UBSan. ~10 s.
RUNNER = {
    "covers": "structure decoders and inspector maths: known files, then hostile ones",
    "gate": True,
}

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
HERE = toolpath.TOOLS
import hostcheck  # noqa: E402

ROOT = os.path.dirname(HERE)
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
       "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer")
SOURCES = ["ubinmap.c", "ubinmap_elf.c", "ubinmap_wad.c", "ubinmap_ttf.c", "ubinmap_disk.c",
           "ubinmap_img.c", "ubyteval.c"]

DRIVER = r"""
#include "base/ubinmap.h"
#include "base/ubyteval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *g_buf;
static size_t g_size;
static int g_violations;

// EXACTLY the file's bytes, so ASan sees an overread, and a request past
// the end is a decoder that did not check first.
static size_t rd(void *ctx, uint64_t off, void *buf, size_t n) {
    (void)ctx;
    if (off > g_size || n > g_size - off) { g_violations++; printf("VIOLATION read %llu+%zu of %zu\n",
                                            (unsigned long long)off, n, g_size); return 0; }
    memcpy(buf, g_buf + off, n);
    return n;
}

static void load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) exit(3);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_size = (size_t)n;
    g_buf = malloc(g_size ? g_size : 1);
    if (g_size && fread(g_buf, 1, g_size, f) != g_size) exit(3);
    fclose(f);
}

static int decode(struct ubinmap *m, int dump) {
    struct ubinmap_src src = { g_size, rd, 0 };
    int ok = ubinmap_decode(&src, m);
    for (int i = 0; i < m->count; i++) {
        const struct ubinmap_node *n = &m->nodes[i];
        if (n->off > g_size || n->len > g_size - n->off) { g_violations++; printf("VIOLATION node %d outside\n", i); }
        if (dump) printf("N|%d|%llu|%llu|%d|%d|%s|%s\n", n->depth, (unsigned long long)n->off,
                         (unsigned long long)n->len, n->tint, n->flags, ubinmap_label(m, i), ubinmap_value(m, i));
    }
    if (dump) printf("F|%s|%s|%d|%s\n", m->kind ? m->kind : "", m->format, m->dropped, m->note);
    return ok;
}

static void tints(struct ubinmap *m) {
    struct ubinmap_span *s;
    int n = ubinmap_tints(m, &s);
    for (int i = 0; i < n; i++) {
        printf("T|%llu|%llu|%d|%d\n", (unsigned long long)s[i].off, (unsigned long long)s[i].len, s[i].tint, s[i].node);
        if (i && s[i].off < s[i - 1].off + s[i - 1].len) { g_violations++; printf("VIOLATION spans overlap\n"); }
        if (ubinmap_span_at(s, n, s[i].off) != i) { g_violations++; printf("VIOLATION span_at\n"); }
    }
    free(s);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "dump")) {
        load(argv[2]);
        struct ubinmap m;
        decode(&m, 1);
        tints(&m);
        char path[256];
        for (int i = 3; i < argc; i++) {
            ubinmap_path(&m, strtoull(argv[i], 0, 0), path, sizeof path);
            printf("P|%s|%s\n", argv[i], path);
        }
        ubinmap_free(&m);
    } else if (argc == 3 && !strcmp(argv[1], "fuzz")) {
        // every prefix, then flips: nothing may read past the end or place a node outside
        load(argv[2]);
        unsigned char *orig = malloc(g_size ? g_size : 1);
        memcpy(orig, g_buf, g_size);
        size_t full = g_size;
        for (size_t len = 0; len <= full; len++) {
            g_size = len;
            struct ubinmap m;
            decode(&m, 0);
            if (m.count) tints(&m);
            ubinmap_free(&m);
        }
        g_size = full;
        srand(7);
        for (int it = 0; it < 3000 && full; it++) {
            memcpy(g_buf, orig, full);
            int flips = 1 + rand() % 6;
            for (int k = 0; k < flips; k++) {
                size_t at = (size_t)rand() % (full < 600 ? full : 600);
                g_buf[at] = (unsigned char)(rand() % 3 == 0 ? 0xFF : rand());
            }
            struct ubinmap m;
            decode(&m, 0);
            struct ubinmap_span *s;
            int n = ubinmap_tints(&m, &s);
            for (int i = 1; i < n; i++)
                if (s[i].off < s[i - 1].off + s[i - 1].len) { g_violations++; printf("VIOLATION fuzz spans\n"); }
            free(s);
            ubinmap_free(&m);
        }
        free(orig);
        printf("fuzzed %zu\n", full);
    } else if (argc == 4 && !strcmp(argv[1], "val")) {      // val HEX be
        uint8_t b[16];
        int n = ubyteval_parse_hex(argv[2], b, 16), be = atoi(argv[3]);
        char out[96];
        for (int r = 0; r < UBV_ROWS; r++) {
            int shown = ubyteval_row(b, n, be, r, out, sizeof out);
            printf("V|%s|%d|%s\n", ubyteval_name(r), shown, out);
        }
        uint64_t off;
        if (ubyteval_as_offset(b, n, be, 0x8000, &off)) printf("O|0x%llX\n", (unsigned long long)off);
        else printf("O|none\n");
    } else if (argc == 5 && !strcmp(argv[1], "off")) {      // off TEXT CUR SIZE
        uint64_t v;
        const char *why = "";
        if (ubyteval_parse_offset(argv[2], strtoull(argv[3], 0, 0), strtoull(argv[4], 0, 0), &v, &why))
            printf("G|%llu\n", (unsigned long long)v);
        else printf("E|%s\n", why);
    } else if (argc == 3 && !strcmp(argv[1], "hex")) {
        uint8_t b[8];
        int n = ubyteval_parse_hex(argv[2], b, 8);
        printf("H|%d|", n);
        for (int i = 0; i < n; i++) printf("%02X", b[i]);
        printf("\n");
    } else if (argc == 3 && !strcmp(argv[1], "copy")) {
        uint8_t b[64];
        int n = ubyteval_parse_hex(argv[2], b, 64);
        char *h = malloc(ubyteval_hex_size((size_t)n)), *a = malloc((size_t)n + 1), *c = malloc(ubyteval_c_size((size_t)n));
        int hn = ubyteval_hex_text(b, (size_t)n, h, ubyteval_hex_size((size_t)n));
        ubyteval_ascii_text(b, (size_t)n, a, (size_t)n + 1);
        int cn = ubyteval_c_array(b, (size_t)n, c, ubyteval_c_size((size_t)n));
        char tiny[4];
        printf("X|%d|%s\nA|%s\nC|%d|%s<END>\nS|%d\n", hn, h, a, cn, c, ubyteval_hex_text(b, (size_t)n, tiny, sizeof tiny));
        free(h); free(a); free(c);
    } else {
        return 2;
    }
    return g_violations ? 1 : 0;
}
"""

# --- the known files ------------------------------------------------------


def elf64():
    """ELF64 LE x86-64: header, 2 phdrs (LOAD r-x, INTERP), a NOTE phdr, interp, a GNU note, 3 sections."""
    interp = b"/lib/ld-toy.so\0"
    note = struct.pack("<III", 4, 16, 3) + b"GNU\0" + bytes(range(16))
    shstr = b"\0.shstrtab\0.text\0"
    text = bytes((i * 7) & 0xFF for i in range(64))
    phoff, phn = 64, 3
    interp_at = phoff + 56 * phn
    note_at = interp_at + 16
    shstr_at = note_at + len(note)
    text_at = shstr_at + len(shstr)
    shoff = text_at + len(text)
    shoff += (-shoff) % 8
    hdr = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    hdr += struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, 0x8000002BC0, phoff, shoff, 0, 64, 56, phn, 64, 3, 1)
    ph = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000, 0x200, 0x200, 0x1000)
    ph += struct.pack("<IIQQQQQQ", 3, 4, interp_at, 0, 0, len(interp), len(interp), 1)
    ph += struct.pack("<IIQQQQQQ", 4, 4, note_at, 0, 0, len(note), len(note), 4)
    body = hdr + ph + interp.ljust(16, b"\0") + note + shstr + text
    body = body.ljust(shoff, b"\0")
    sh = bytes(64)
    sh += struct.pack("<IIQQQQIIQQ", 1, 3, 0, 0, shstr_at, len(shstr), 0, 0, 1, 0)
    sh += struct.pack("<IIQQQQIIQQ", 11, 1, 6, 0x401000, text_at, len(text), 0, 0, 16, 0)
    return body + sh, dict(interp_at=interp_at, note_at=note_at, text_at=text_at, shoff=shoff,
                           shstr_at=shstr_at)


def elf32_be():
    hdr = b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(8)
    hdr += struct.pack(">HHIIIIIHHHHHH", 2, 20, 1, 0x10000074, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    ph = struct.pack(">IIIIIIII", 1, 0, 0x10000000, 0x10000000, 0x80, 0x90, 7, 0x10000)
    return hdr + ph + bytes(44)


def wad():
    lumps = [(b"PLAYPAL", bytes(range(32))), (b"E1M1", b""), (b"THINGS", bytes(40))]
    data, dirents, at = b"", b"", 12
    for name, body in lumps:
        dirents += struct.pack("<ii", at if body else 0, len(body)) + name.ljust(8, b"\0")
        data += body
        at += len(body)
    return b"PWAD" + struct.pack("<ii", len(lumps), 12 + len(data)) + data + dirents


def ttf():
    tables = [(b"cmap", bytes(20)), (b"head", bytes(54))]
    n = len(tables)
    off = 12 + 16 * n
    dirr, data = b"", b""
    for tag, body in tables:
        dirr += tag + struct.pack(">III", 0x1234, off + len(data), len(body))
        data += body
    return struct.pack(">IHHHH", 0x00010000, n, 32, 1, 0) + dirr + data


def mbr(parts, size=512):
    b = bytearray(size)
    b[:4] = b"\xfa\x33\xc0\x8e"
    struct.pack_into("<I", b, 440, 0xCAFEF00D)
    for i, (boot, typ, start, count) in enumerate(parts):
        struct.pack_into("<B3sB3sII", b, 446 + 16 * i, boot, b"\0\0\0", typ, b"\0\0\0", start, count)
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def guid_bytes(s):
    p = s.split("-")
    return (struct.pack("<IHH", int(p[0], 16), int(p[1], 16), int(p[2], 16))
            + bytes.fromhex(p[3]) + bytes.fromhex(p[4]))


def gpt():
    b = bytearray(mbr([(0, 0xEE, 1, 0xFFFFFFFF)], 512 * 6))
    ents = [("C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system partition", 34, 2081),
            ("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "SaunaOS", 2082, 10000)]
    hdr = b"EFI PART" + struct.pack("<IIII", 0x10000, 92, 0, 0) + struct.pack("<QQQQ", 1, 999, 34, 990)
    hdr += guid_bytes("11223344-5566-7788-99AA-BBCCDDEEFF00") + struct.pack("<QIII", 2, 4, 128, 0)
    b[512:512 + len(hdr)] = hdr
    for i, (t, name, lo, hi) in enumerate(ents):
        e = guid_bytes(t) + guid_bytes("00000000-0000-0000-0000-%012X" % (i + 1)) + struct.pack("<QQQ", lo, hi, 0)
        e += name.encode("utf-16-le").ljust(72, b"\0")
        b[1024 + 128 * i:1024 + 128 * (i + 1)] = e
    return bytes(b)


def fat(bits):
    b = bytearray(512 * 80)
    b[0:3] = b"\xeb\x3c\x90"
    b[3:11] = b"SAUNAOS "
    if bits == 16:
        struct.pack_into("<HBHBHHBHHHII", b, 11, 512, 4, 4, 2, 512, 20000, 0xF8, 20, 32, 2, 0, 0)
        b[36] = 0x80
        b[38] = 0x29
        struct.pack_into("<I", b, 39, 0x12345678)
        b[43:54] = b"TESTVOL    "
        b[54:62] = b"FAT16   "
    else:
        struct.pack_into("<HBHBHHBHHHII", b, 11, 512, 8, 32, 2, 0, 0, 0xF8, 0, 32, 2, 0, 600000)
        struct.pack_into("<IHHIHH", b, 36, 580, 0, 0, 2, 1, 6)
        b[64] = 0x80
        b[66] = 0x29
        struct.pack_into("<I", b, 67, 0xABCDEF01)
        b[71:82] = b"BIGVOL     "
        b[82:90] = b"FAT32   "
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def qoi():
    return b"qoif" + struct.pack(">IIBB", 3, 2, 4, 0) + bytes([0xFE, 1, 2, 3] * 6) + bytes(7) + b"\x01"


def bmp():
    pix = bytes(range(48))
    dib = struct.pack("<IiiHHIIiiII", 40, 4, -4, 1, 24, 0, len(pix), 2835, 2835, 0, 0)
    return b"BM" + struct.pack("<IHHI", 14 + 40 + len(pix), 0, 0, 54) + dib + pix


# --- running ----------------------------------------------------------------


def build(tmp, patch=None):
    drv = hostcheck.write(tmp, "ubinmap_host.c", DRIVER)
    srcs = []
    for name in SOURCES:
        edits = [e for f, e in (patch or []) if f == name]
        srcs.append(hostcheck.stage(tmp, "userland/libs/base/" + name, dest="base/" + name, edits=edits,
                                    apply=bool(edits), tool="ubinmap_hostcheck"))
    return hostcheck.compile(tmp, "ubinmap_host", [drv, *srcs], flags=SAN,
                             includes=[os.path.join(ROOT, "userland", "libs")], tool="ubinmap_hostcheck")


def run(exe, *args):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
    r = subprocess.run([exe, *[str(a) for a in args]], capture_output=True, env=env, timeout=900)
    out = r.stdout.decode("utf-8", "replace")
    return r.returncode, out, r.stderr.decode("utf-8", "replace")


class Dump:
    def __init__(self, out):
        self.nodes, self.tints, self.paths = [], [], {}
        self.kind = self.format = self.note = ""
        self.dropped = 0
        for ln in out.splitlines():
            p = ln.split("|")
            if p[0] == "N":
                self.nodes.append(dict(depth=int(p[1]), off=int(p[2]), len=int(p[3]), tint=int(p[4]),
                                       flags=int(p[5]), label=p[6], value="|".join(p[7:])))
            elif p[0] == "F":
                self.kind, self.format, self.dropped, self.note = p[1], p[2], int(p[3]), "|".join(p[4:])
            elif p[0] == "T":
                self.tints.append(tuple(int(x) for x in p[1:]))
            elif p[0] == "P":
                self.paths[p[1]] = p[2]

    def find(self, label, depth=None):
        return [n for n in self.nodes if n["label"] == label and (depth is None or n["depth"] == depth)]


def dump(exe, tmp, name, data, *paths):
    path = os.path.join(tmp, name)
    open(path, "wb").write(data)
    rc, out, err = run(exe, "dump", path, *paths)
    if rc != 0:
        raise RuntimeError("%s: driver exit %d\n%s%s" % (name, rc, out[-800:], err[-1500:]))
    return Dump(out)


def want(problems, ok, what):
    if not ok:
        problems.append(what)


def node_is(problems, d, label, value=None, off=None, length=None, depth=None, tag=""):
    hits = d.find(label, depth)
    if not hits:
        problems.append("%s: no node %r (have %s)" % (tag, label, [n["label"] for n in d.nodes][:40]))
        return None
    for n in hits:
        if ((value is None or n["value"] == value) and (off is None or n["off"] == off)
                and (length is None or n["len"] == length)):
            return n
    problems.append("%s: node %r is %s, wanted value=%r off=%r len=%r" % (tag, label, hits[:3], value, off, length))
    return None


def check_known(exe, tmp, problems):
    data, at = elf64()
    d = dump(exe, tmp, "a.elf", data, "0x18", str(at["interp_at"] + 2), "0x40", "99999999")
    t = "elf64"
    want(problems, d.kind == "elf" and d.format == "ELF64 executable, x86-64", "%s: format %r" % (t, d.format))
    node_is(problems, d, "ELF header", off=0, length=64, depth=0, tag=t)
    node_is(problems, d, "Entry", "0x8000002BC0", 24, 8, 1, tag=t)
    node_is(problems, d, "Machine", "x86-64 (62)", 18, 2, tag=t)
    node_is(problems, d, "LOAD r-x", "0x0000", 64, 56, 1, tag=t)
    node_is(problems, d, "INTERP r--", None, 120, 56, 1, tag=t)
    node_is(problems, d, "Interpreter", "/lib/ld-toy.so", at["interp_at"], 15, 0, tag=t)
    node_is(problems, d, "GNU", "type 3, 16 bytes", at["note_at"], 12 + 4 + 16, 1, tag=t)
    node_is(problems, d, ".text", "PROGBITS", at["shoff"] + 128, 64, 1, tag=t)
    node_is(problems, d, ".shstrtab", "STRTAB", at["shoff"] + 64, 64, 1, tag=t)
    node_is(problems, d, "Contents", "0x%X" % at["text_at"], at["text_at"], 64, 2, tag=t)
    node_is(problems, d, "File size", "512", 64 + 32, 8, 2, tag=t)
    want(problems, d.paths.get("0x18") == "ELF header > Entry", "%s: path at 0x18 is %r" % (t, d.paths.get("0x18")))
    want(problems, d.paths.get(str(at["interp_at"] + 2)) == "Interpreter", "%s: path in the interpreter %r"
         % (t, d.paths.get(str(at["interp_at"] + 2))))
    want(problems, d.paths.get("0x40") == "Program headers > LOAD r-x > Type", "%s: path at 0x40 %r"
         % (t, d.paths.get("0x40")))
    want(problems, d.paths.get("99999999") == "", "%s: a path past the end" % t)
    want(problems, not d.note, "%s: a clean file has a note: %r" % (t, d.note))
    # the tints: disjoint, sorted, the header's slot over its bytes, the program header entries
    # alternating, the interpreter's own
    tint_at = {}
    for off, ln, tint, _node in d.tints:
        for k in (off, off + ln - 1):
            tint_at[k] = tint
    for off, tint in ((0, 1), (63, 1), (64, 2), (120, 3), (176, 2), (at["interp_at"], 4), (at["note_at"], 5)):
        want(problems, tint_at.get(off) == tint, "%s: tint at %d is %r, wanted %d (runs %s)"
             % (t, off, tint_at.get(off), tint, d.tints[:8]))
    spans = sorted(d.tints)
    want(problems, all(a[0] + a[1] <= b[0] for a, b in zip(spans, spans[1:])), "%s: tinted runs overlap" % t)

    d = dump(exe, tmp, "b.elf", elf32_be())
    t = "elf32be"
    want(problems, d.format == "ELF32 executable, PowerPC", "%s: format %r" % (t, d.format))
    node_is(problems, d, "Entry", "0x10000074", 24, 4, 1, tag=t)
    node_is(problems, d, "LOAD rwx", "0x0000", 52, 32, 1, tag=t)
    node_is(problems, d, "Memory size", "144", 52 + 20, 4, 2, tag=t)
    node_is(problems, d, "Data", "big-endian", tag=t)

    d = dump(exe, tmp, "a.wad", wad())
    t = "wad"
    want(problems, d.format == "DOOM PWAD, 3 lumps", "%s: format %r" % (t, d.format))
    node_is(problems, d, "Lumps", "3", depth=1, tag=t)
    node_is(problems, d, "PLAYPAL", "32 bytes", 12, 32, 1, tag=t)
    node_is(problems, d, "E1M1", "0 bytes", None, 0, 1, tag=t)
    node_is(problems, d, "THINGS", "40 bytes", 44, 40, 1, tag=t)
    node_is(problems, d, "Directory", "3 entries", 84, 48, 0, tag=t)
    lumps = d.find("Lumps", 0)
    want(problems, lumps and lumps[0]["off"] == 12 and lumps[0]["len"] == 72, "%s: Lumps spans the data %s" % (t, lumps))

    d = dump(exe, tmp, "a.ttf", ttf())
    t = "ttf"
    want(problems, d.format == "TrueType font, 2 tables", "%s: format %r" % (t, d.format))
    node_is(problems, d, "cmap", "20 bytes", 44, 20, 1, tag=t)
    node_is(problems, d, "head", "54 bytes", 64, 54, 1, tag=t)
    node_is(problems, d, "Checksum", "0x00001234", tag=t)

    d = dump(exe, tmp, "a.mbr", mbr([(0x80, 0x0C, 2048, 204800), (0, 0x83, 206848, 4096)]))
    t = "mbr"
    want(problems, d.format == "MBR disk, 2 partitions", "%s: format %r" % (t, d.format))
    node_is(problems, d, "Partition 1", "0x0C FAT32 (LBA)", 446, 16, 1, tag=t)
    node_is(problems, d, "Partition 2", "0x83 Linux", 462, 16, 1, tag=t)
    node_is(problems, d, "Boot flag", "active", 446, 1, 2, tag=t)
    node_is(problems, d, "Sectors", "204800 (100.0 MiB)", 446 + 12, 4, 2, tag=t)
    node_is(problems, d, "Disk signature", "0xCAFEF00D", 440, 4, tag=t)

    d = dump(exe, tmp, "a.gpt", gpt())
    t = "gpt"
    want(problems, d.format == "GPT disk, 2 partitions", "%s: format %r" % (t, d.format))
    node_is(problems, d, "EFI system partition", "EFI System", 1024, 128, 1, tag=t)
    node_is(problems, d, "SaunaOS", "Basic data", 1152, 128, 1, tag=t)
    node_is(problems, d, "Disk GUID", "11223344-5566-7788-99AA-BBCCDDEEFF00", 512 + 56, 16, tag=t)
    node_is(problems, d, "Size", "1.0 MiB", tag=t)
    node_is(problems, d, "Entries", "4", tag=t)

    d = dump(exe, tmp, "a.fat16", fat(16))
    t = "fat16"
    want(problems, d.format == "FAT16 volume", "%s: format %r" % (t, d.format))
    node_is(problems, d, "Label", "TESTVOL", 43, 11, tag=t)
    node_is(problems, d, "FAT 2", "20 sectors", 4 * 512 + 20 * 512, 20 * 512, tag=t)
    node_is(problems, d, "Root directory", "512 entries", tag=t)
    d = dump(exe, tmp, "a.fat32", fat(32))
    t = "fat32"
    want(problems, d.format == "FAT32 volume", "%s: format %r" % (t, d.format))
    node_is(problems, d, "Label", "BIGVOL", 71, 11, tag=t)
    node_is(problems, d, "Root cluster", "2", 44, 4, tag=t)

    d = dump(exe, tmp, "a.qoi", qoi())
    want(problems, d.format == "QOI picture, 3 x 2, RGBA", "qoi: format %r" % d.format)
    node_is(problems, d, "End marker", None, len(qoi()) - 8, 8, tag="qoi")
    d = dump(exe, tmp, "a.bmp", bmp())
    want(problems, d.format == "BMP picture, 4 x 4, 24-bit", "bmp: format %r" % d.format)
    node_is(problems, d, "Height", "-4 (top-down)", 22, 4, tag="bmp")
    node_is(problems, d, "Pixels", "48 bytes", 54, 48, tag="bmp")

    d = dump(exe, tmp, "plain.txt", b"hello, this is not any format at all\n" * 20)
    want(problems, d.kind == "" and not d.nodes, "text: matched %r with %d nodes" % (d.kind, len(d.nodes)))


def check_hostile(exe, tmp, problems):
    """Lengths past the end, huge counts, offsets that wrap: refused or clipped, never read past."""
    data, at = elf64()
    b = bytearray(data)
    struct.pack_into("<H", b, 56, 0xFFFF)                       # phnum 65535
    struct.pack_into("<Q", b, 40, 0xFFFFFFFFFFFFFF00)           # shoff near 2^64
    d = dump(exe, tmp, "h1.elf", bytes(b))
    want(problems, d.note != "", "hostile elf (phnum 65535, shoff wraps): no note")
    want(problems, len(d.nodes) < 70000, "hostile elf: %d nodes" % len(d.nodes))
    b = bytearray(data)
    struct.pack_into("<I", b, at["shoff"] + 128, 0xFFFFFFF0)     # .text's name far past .shstrtab
    struct.pack_into("<QQ", b, 64 + 8, 0xFFFFFFFFFFFFFFF0, 0)     # LOAD's offset wraps
    struct.pack_into("<H", b, 54, 8)                            # ...and then entries too small
    d = dump(exe, tmp, "h2.elf", bytes(b))
    want(problems, "too small" in d.note, "hostile elf: entries too small not said (%r)" % d.note)
    # .text's name one byte PAST .shstrtab, still inside the file (in .text's own bytes): a reader
    # that bounds it by the file and not by the table reads a name there
    b = bytearray(data)
    struct.pack_into("<I", b, at["shoff"] + 128, at["text_at"] - at["shstr_at"] + 1)
    d = dump(exe, tmp, "h3.elf", bytes(b))
    want(problems, d.find("[2]", 1), "hostile elf: a name outside .shstrtab is not read (%s)"
         % [n["label"] for n in d.nodes if n["depth"] == 1][-3:])

    w = bytearray(wad())
    struct.pack_into("<i", w, 4, 0x7FFFFFFF)
    d = dump(exe, tmp, "h1.wad", bytes(w))
    want(problems, "claims 2147483647 lumps; the file holds 3" in d.note, "hostile wad count: note %r" % d.note)
    want(problems, d.format == "DOOM PWAD, 3 lumps" and node_is(problems, d, "Lumps", "3", depth=0, tag="h1.wad"),
         "hostile wad count: the listing says what the file holds (%r)" % d.format)
    # two lumps OVERLAPPING: where both cover a byte, the later one's tint is the one shown
    w = bytearray(wad())
    w[4:8] = struct.pack("<i", 4)
    w += struct.pack("<ii", 20, 8) + b"OVER\0\0\0\0"
    d = dump(exe, tmp, "h4.wad", bytes(w))
    tint_of = {}
    for off, ln, tint, _n in d.tints:
        for k in range(off, off + ln):
            tint_of[k] = tint
    want(problems, (tint_of.get(12), tint_of.get(20), tint_of.get(27), tint_of.get(28)) == (3, 4, 4, 3),
         "overlapping lumps: the later wins its bytes (tints at 12/20/27/28: %s)"
         % [tint_of.get(k) for k in (12, 20, 27, 28)])
    w = bytearray(wad())
    struct.pack_into("<i", w, 8, -5)
    d = dump(exe, tmp, "h2.wad", bytes(w))
    want(problems, "negative" in d.note and d.format.endswith("damaged"), "negative wad dir: %r %r" % (d.format, d.note))
    w = bytearray(wad())
    struct.pack_into("<II", w, 84 + 32, 0xFFFFFFF0, 0x100)      # THINGS at 2^32-16, 256 bytes: wraps a u32
    d = dump(exe, tmp, "h3.wad", bytes(w))
    things = d.find("THINGS", 1)
    want(problems, things and things[0]["len"] == 0 and things[0]["flags"] == 1,
         "a lump past the end is clipped and flagged: %s" % things)
    big = b"IWAD" + struct.pack("<ii", 100000, 12) + struct.pack("<ii8s", 0, 0, b"MARK") * 100000
    d = dump(exe, tmp, "big.wad", big)
    want(problems, d.dropped > 0 and len(d.nodes) == 65536, "100000 lumps: %d nodes, %d dropped (want the cap to hold)"
         % (len(d.nodes), d.dropped))

    f = bytearray(ttf())
    struct.pack_into(">H", f, 4, 500)
    d = dump(exe, tmp, "h1.ttf", bytes(f))
    want(problems, "claims 500 tables" in d.note, "hostile ttf count: %r" % d.note)
    f = bytearray(ttf())
    struct.pack_into(">II", f, 12 + 8, 0xFFFFFFFF, 0xFFFFFFFF)
    d = dump(exe, tmp, "h2.ttf", bytes(f))
    want(problems, "past the end" in d.note, "a table past the end: %r" % d.note)

    g = bytearray(gpt())
    struct.pack_into("<QI", g, 512 + 72, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF)   # entries at a wrapping LBA, 2^32-1 of them
    d = dump(exe, tmp, "h1.gpt", bytes(g))
    want(problems, "claims 4294967295 entries" in d.note, "hostile gpt: %r" % d.note)
    g = bytearray(gpt())
    struct.pack_into("<I", g, 512 + 84, 0)
    d = dump(exe, tmp, "h2.gpt", bytes(g))
    want(problems, "entry size of 0" in d.note, "gpt entry size 0: %r" % d.note)

    v = bytearray(fat(16))
    struct.pack_into("<H", v, 22, 0xFFFF)          # sectors per FAT far past the file
    d = dump(exe, tmp, "h1.fat", bytes(v))
    fats = d.find("FAT 2")
    want(problems, fats and fats[0]["flags"] == 1, "a FAT past the end is clipped: %s" % fats)

    samples = dict(e64=elf64()[0], e32=elf32_be(), wad=wad(), ttf=ttf(),
                   mbr=mbr([(0x80, 0x0C, 2048, 204800)]), gpt=gpt()[:1600], fat=fat(16)[:600],
                   qoi=qoi(), bmp=bmp())
    for name, body in samples.items():
        p = os.path.join(tmp, "fz-" + name)
        open(p, "wb").write(body)
        rc, out, err = run(exe, "fuzz", p)
        if rc != 0:
            problems.append("fuzz %s: exit %d\n%s%s" % (name, rc, out[-600:], err[-1500:]))


VALUES = [
    # (hex, big-endian, {row: (shown, text)})
    ("C0 2B 00 00 00 00 00 00", 0, {"Binary": (1, "1100 0000"), "Int8": (1, "-64 (unsigned 192)"),
                                    "Int16": (1, "11200"), "Int32": (1, "11200"), "Int64": (1, "11200"),
                                    "Hex 64": (1, "0x0000000000002BC0"), "UTF-8": (1, "not a character (C0)")}),
    ("C0 2B 00 00", 1, {"Int16": (1, "-16341 (unsigned 49195)"), "Int32": (1, "-1070923776 (unsigned 3224043520)"),
                        "Int64": (0, "needs 8 bytes, 4 left"), "Float64": (0, "needs 8 bytes, 4 left")}),
    ("CD CC CC 3D", 0, {"Float32": (1, "0.1")}),
    ("3D CC CC CD", 1, {"Float32": (1, "0.1")}),
    ("9A 99 99 99 99 99 B9 3F", 0, {"Float64": (1, "0.1"), "Int64": (1, "4591870180066957722")}),
    ("00 00 C0 7F", 0, {"Float32": (1, "NaN")}),
    ("00 00 80 FF", 0, {"Float32": (1, "-infinity")}),
    ("FF FF FF FF FF FF FF FF", 0, {"Int64": (1, "-1 (unsigned 18446744073709551615)")}),
    ("00 00 00 00 00 00 00 80", 0, {"Int64": (1, "-9223372036854775808 (unsigned 9223372036854775808)")}),
    ("E2 82 AC 00", 0, {"UTF-8": (1, "€  U+20AC")}),
    ("E2 28 A1", 0, {"UTF-8": (1, "not a character (E2 28)")}),
    ("E2 82", 0, {"UTF-8": (1, "cut short (E2 82)")}),
    ("BF", 0, {"UTF-8": (1, "not a character (BF)"), "Int16": (0, "needs 2 bytes, 1 left")}),
    ("ED A0 80", 0, {"UTF-8": (1, "not a character (ED A0)")}),
    ("0A", 0, {"UTF-8": (1, "U+000A (control)")}),
    ("41", 0, {"UTF-8": (1, "A  U+0041")}),
]


def check_values(exe, problems):
    for hexs, be, rows in VALUES:
        rc, out, err = run(exe, "val", hexs, be)
        if rc != 0:
            problems.append("val %s: exit %d %s" % (hexs, rc, err[-600:]))
            continue
        got = {}
        for ln in out.splitlines():
            p = ln.split("|")
            if p[0] == "V":
                got[p[1]] = (int(p[2]), p[3])
        for row, exp in rows.items():
            want(problems, got.get(row) == exp, "value %s %s %s: got %r, wanted %r"
                 % (hexs, "BE" if be else "LE", row, got.get(row), exp))
    for hexs, be, exp in (("C0 2B 00 00 00 00 00 00", 0, "0x2BC0"), ("00 00 2B C0", 1, "0x2BC0"),
                          ("34 12 FF FF", 0, "0x1234"), ("FF FF FF FF FF FF FF FF", 0, "none")):
        rc, out, _ = run(exe, "val", hexs, be)
        line = [ln for ln in out.splitlines() if ln.startswith("O|")]
        want(problems, line and line[0] == "O|" + exp, "as an offset %s: %s, wanted %s" % (hexs, line, exp))
    for text, cur, size, exp in (("0x2BC0", 0, 100000, "G|11200"), ("11200", 0, 100000, "G|11200"),
                                 ("+16", 100, 1000, "G|116"), ("-0x10", 100, 1000, "G|84"),
                                 ("2bc0h", 0, 100000, "G|11200"), ("  42  ", 0, 100, "G|42"),
                                 ("-200", 100, 1000, "E|That is before the start of the file."),
                                 ("1000", 0, 1000, "E|That is past the end of the file."),
                                 ("0xZZ", 0, 10, "E|Type an offset: 0x2BC0, 11200, +16 or -0x10."),
                                 ("99999999999999999999999", 0, 10, "E|That number is too large."),
                                 ("+1", 0xFFFFFFFFFFFFFFFF, 10, "E|That is past the end of the file."),
                                 ("0", 0, 0, "E|The file is empty.")):
        rc, out, _ = run(exe, "off", text, cur, size)
        want(problems, out.strip() == exp, "offset %r from %d in %d: %r, wanted %r" % (text, cur, size, out.strip(), exp))
    for text, exp in (("C0 2B", "H|2|C02B"), ("c02b", "H|2|C02B"), ("0xC0,0x2B", "H|2|C02B"),
                      ("C0 2", "H|-1|"), ("hello", "H|-1|"), ("", "H|0|"), ("00 11 22 33 44 55 66 77 88", "H|-1|")):
        rc, out, _ = run(exe, "hex", text)
        want(problems, out.strip() == exp, "hex %r: %r, wanted %r" % (text, out.strip(), exp))
    rc, out, _ = run(exe, "copy", " ".join("%02X" % ((i * 37 + 0x41) & 0xFF) for i in range(13)))
    exp_c = ("unsigned char data[13] = {\n    "
             + ", ".join("0x%02x" % ((i * 37 + 0x41) & 0xFF) for i in range(12)) + ",\n    0x%02x,\n};\n"
             % ((12 * 37 + 0x41) & 0xFF))
    lines = out.split("\n")
    want(problems, lines[0] == "X|38|" + " ".join("%02X" % ((i * 37 + 0x41) & 0xFF) for i in range(13)),
         "hex copy: %r" % lines[0])
    want(problems, lines[1] == "A|" + "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in
                                                ((i * 37 + 0x41) & 0xFF for i in range(13))), "text copy: %r" % lines[1])
    cpart = out.split("C|", 1)[1].split("<END>")[0]
    want(problems, cpart == "%d|%s" % (len(exp_c), exp_c), "C array: %r, wanted %r" % (cpart, exp_c))
    want(problems, "S|-1" in out, "a hex copy that does not fit is refused, not cut")


def problems_of(exe, tmp, half):
    problems = []
    try:
        if half in ("known", "all"):
            check_known(exe, tmp, problems)
        if half in ("hostile", "all"):
            check_hostile(exe, tmp, problems)
        if half in ("values", "all"):
            check_values(exe, problems)
    except RuntimeError as e:
        problems.append(str(e)[:1500])
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    if args.positive_control:
        return positive_control()
    random.seed(1)
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        problems = problems_of(exe, tmp, "all")
    if problems:
        for p in problems:
            print("FAIL:", p)
        print("ubinmap_hostcheck: %d problems" % len(problems))
        return 1
    print("ubinmap_hostcheck: ok (9 formats known, hostile files refused or clipped, inspector rows exact)")
    return 0


def positive_control():
    controls = [
        ("a bounds check that always agrees", "hostile",
         [("ubinmap.c", ("if (off > size || n > size - off) return -1;", "(void)size;"))]),
        ("a node's range not clipped to the file", "hostile",
         [("ubinmap.c", ("    } else if (len > size - off) {\n        n->flags |= UBINMAP_CLIPPED;\n        len = size - off;\n    }",
                         "    }"))]),
        ("the entry read from the wrong offset", "known",
         [("ubinmap_elf.c", ("entry = bm_get64(c, h + 24);", "entry = bm_get64(c, h + 20);"))]),
        ("the WAD directory trusted past the file", "hostile",
         [("ubinmap_wad.c", ("uint64_t n = (uint64_t)lumps < fit ? (uint64_t)lumps : fit;",
                             "uint64_t n = (uint64_t)lumps;"))]),
        ("the tint winner inverted", "hostile",
         [("ubinmap.c", ("return a->pri_depth != b->pri_depth ? a->pri_depth > b->pri_depth : a->idx > b->idx;",
                         "return a->pri_depth != b->pri_depth ? a->pri_depth < b->pri_depth : a->idx < b->idx;"))]),
        ("a section name read past .shstrtab", "hostile",
         [("ubinmap_elf.c", ("if (str_len && name < str_len) {", "if (str_len) {"))]),
        ("the integer's sign lost", "values",
         [("ubyteval.c", ("    if (!(u & sign)) {", "    if (1) {"))]),
        ("the node cap not enforced", "hostile",
         [("ubinmap.c", ("if (m->count >= UBINMAP_NODES_MAX) { m->dropped++; return -1; }", ""))]),
    ]
    rc = 0
    for name, half, patch in controls:
        with tempfile.TemporaryDirectory() as tmp:
            exe = build(tmp, patch)
            problems = problems_of(exe, tmp, half)
        if problems:
            print("control %-40s RED (good): %s" % (name, problems[0].splitlines()[0][:110]))
        else:
            print("control %-40s STAYED GREEN -- this check cannot see that break" % name)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
