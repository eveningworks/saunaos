#!/usr/bin/env python3
"""tools/gen_ne.py -- build a Win16 (NE) executable from one NASM source.

w16 (docs/w16-design.md) needs test programs, and the project ships no
Microsoft binary and needs no foreign toolchain: a test program is a
NASM source using userland/w16/samples/ne16.inc, assembled once per
segment, and this script lays the result out as an NE file -- an MZ
stub, the NE header and its tables, the segments and their relocations.

    python3 tools/gen_ne.py userland/w16/samples/hello.asm -o hello.exe
    python3 tools/gen_ne.py --dump hello.exe      # print what it holds

THE RELOCATION CHAIN. A non-additive NE relocation names the FIRST site
of a chain: the word at each site holds the offset of the next one, and
0xFFFF ends it. Every site here is its own chain of one, so the word at
the site is set to 0xFFFF -- a loader that ignores chains still reads
the right sites, and one that follows them stops at once.

--dump is a second, independent reading of the format for the tests:
w16's own parser (userland/w16/ne.c) is checked against it.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

REL_IMPORT, REL_INTERNAL, REL_ENTRY, REL_EXPORT = 1, 2, 3, 4
SRC_SEGMENT, SRC_FARPTR, SRC_OFFSET = 2, 3, 5

SEG_DATA = 0x0001
SEG_MOVABLE = 0x0010
SEG_PRELOAD = 0x0040
SEG_RELOCS = 0x0100

ALIGN_SHIFT = 4          # segments start on 16-byte boundaries
NE_OFFSET = 0x80         # the NE header follows a 128-byte MZ stub


def directives(path):
    """The ;w16: lines: module, segments, autodata, stack, heap."""
    d = {"module": None, "segments": {}, "autodata": 0, "stack": 4096, "heap": 1024,
         "description": ""}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*;w16:\s*(\w+)\s*(.*?)\s*$", line)
        if not m:
            continue
        key, val = m.group(1), m.group(2)
        if key == "segment":
            n, kind = val.split()
            d["segments"][int(n)] = kind
        elif key in ("autodata", "stack", "heap"):
            d[key] = int(val, 0)
        elif key in ("module", "description"):
            d[key] = val
        else:
            sys.exit(f"gen_ne: {path}: unknown directive '{key}'")
    if not d["module"] or not d["segments"]:
        sys.exit(f"gen_ne: {path}: needs ';w16: module' and at least one ';w16: segment'")
    if sorted(d["segments"]) != list(range(1, len(d["segments"]) + 1)):
        sys.exit(f"gen_ne: {path}: segments must be numbered 1..n")
    return d


def assemble(src, n, tmp):
    out = os.path.join(tmp, f"seg{n}.bin")
    inc = os.path.dirname(os.path.abspath(src)) + os.sep
    r = subprocess.run(["nasm", "-f", "bin", "-w-other", f"-DW16_PASS={n}", "-I", inc,
                        "-o", out, src], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"gen_ne: nasm failed on segment {n}:\n{r.stderr}")
    blob = open(out, "rb").read()
    (rel_at,) = struct.unpack_from("<I", blob, len(blob) - 4)
    return bytearray(blob[:rel_at]), blob[rel_at:len(blob) - 4]


def parse_relocs(raw, segno):
    """The records ne16.inc wrote for one segment."""
    relocs, entry, exports, i = [], None, [], 0
    while i < len(raw):
        kind = raw[i]
        i += 1
        if kind == REL_IMPORT:
            site, src, ln = struct.unpack_from("<HBB", raw, i)
            i += 4
            mod = raw[i:i + ln].decode("ascii")
            i += ln
            (ordinal,) = struct.unpack_from("<H", raw, i)
            i += 2
            relocs.append(("import", site, src, mod.upper(), ordinal))
        elif kind == REL_INTERNAL:
            site, src, seg, off = struct.unpack_from("<HBBH", raw, i)
            i += 6
            relocs.append(("internal", site, src, seg, off))
        elif kind == REL_ENTRY:
            (off,) = struct.unpack_from("<H", raw, i)
            i += 2
            entry = (segno, off)
        elif kind == REL_EXPORT:
            ordinal, off, ln = struct.unpack_from("<HHB", raw, i)
            i += 5
            exports.append((ordinal, raw[i:i + ln].decode("ascii").upper(), segno, off))
            i += ln
        else:
            sys.exit(f"gen_ne: segment {segno}: bad relocation record kind {kind}")
    return relocs, entry, exports


def pstr(s):
    b = s.encode("ascii")
    return bytes([len(b)]) + b


def mz_stub():
    """A DOS stub that prints a line and exits with 1."""
    code = bytes([0x0E, 0x1F,                    # push cs; pop ds
                  0xBA, 0x0E, 0x00,              # mov dx, msg (offset 0x0E)
                  0xB4, 0x09, 0xCD, 0x21,        # mov ah,9; int 21h
                  0xB8, 0x01, 0x4C, 0xCD, 0x21])  # mov ax,4C01h; int 21h
    code += b"Needs a Win16 host (toy-os w16).\r\n$"
    hdr = bytearray(0x40)
    total = 0x40 + len(code)
    struct.pack_into("<2sHHHHHHHHHHHHH", hdr, 0, b"MZ", total % 512, (total + 511) // 512,
                     0, 4, 0, 0xFFFF, 0, 0xB8, 0, 0, 0, 0x40, 0)
    struct.pack_into("<I", hdr, 0x3C, NE_OFFSET)
    stub = bytes(hdr) + code
    if len(stub) > NE_OFFSET:
        sys.exit("gen_ne: MZ stub too long")
    return stub + bytes(NE_OFFSET - len(stub))


def build(src, out):
    d = directives(src)
    nseg = len(d["segments"])
    segs, relocs, exports, entry = {}, {}, [], None
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(1, nseg + 1):
            data, raw = assemble(src, n, tmp)
            r, e, x = parse_relocs(raw, n)
            segs[n], relocs[n] = data, r
            exports += x
            if e:
                if entry:
                    sys.exit("gen_ne: two W16_ENTRY points")
                entry = e
    if not entry:
        sys.exit("gen_ne: no W16_ENTRY")

    # Module references, in order of first use.
    modules = []
    for n in range(1, nseg + 1):
        for r in relocs[n]:
            if r[0] == "import" and r[3] not in modules:
                modules.append(r[3])

    # Tables after the 64-byte NE header, in the order Microsoft's
    # linker wrote them: segment, resource, resident names, module
    # references, imported names, entry; non-resident names last.
    seg_table_off = 0x40
    res_table_off = seg_table_off + 8 * nseg
    resource_table = struct.pack("<H", ALIGN_SHIFT) + struct.pack("<H", 0)   # no resources
    resident = pstr(d["module"].upper()) + struct.pack("<H", 0)
    for ordinal, name, _, _ in sorted(exports):
        resident += pstr(name) + struct.pack("<H", ordinal)
    resident += b"\0"
    imported = b"\0"            # offset 0 is an empty name, as linkers wrote it
    modref = b""
    for m in modules:
        modref += struct.pack("<H", len(imported))
        imported += pstr(m)
    # Entry table: one bundle of fixed entries per run of ordinals in the
    # same segment, ordinal 1 upwards; unused ordinals as empty bundles.
    entry_table = b""
    by_ord = {o: (s, off) for o, _, s, off in exports}
    o, top = 1, max(by_ord) if by_ord else 0
    while o <= top:
        if o not in by_ord:
            gap = 0
            while o + gap <= top and (o + gap) not in by_ord and gap < 255:
                gap += 1
            entry_table += bytes([gap, 0])
            o += gap
            continue
        seg = by_ord[o][0]
        run = []
        while o <= top and o in by_ord and by_ord[o][0] == seg and len(run) < 255:
            run.append(by_ord[o][1])
            o += 1
        entry_table += bytes([len(run), seg])
        for off in run:
            entry_table += struct.pack("<BH", 0x01, off)    # exported
    entry_table += b"\0"

    resident_off = res_table_off + len(resource_table)
    modref_off = resident_off + len(resident)
    imported_off = modref_off + len(modref)
    entry_off = imported_off + len(imported)
    nonres = pstr(d["description"] or d["module"]) + struct.pack("<H", 0) + b"\0"
    tables_end = NE_OFFSET + entry_off + len(entry_table)
    nonres_file_off = tables_end

    # Segment data, each aligned, followed by its relocation records.
    pos = nonres_file_off + len(nonres)
    seg_entries, body = [], bytearray()
    for n in range(1, nseg + 1):
        pad = (-pos) % (1 << ALIGN_SHIFT)
        body += bytes(pad)
        pos += pad
        data = segs[n]
        recs = b""
        for r in relocs[n]:
            if r[0] == "import":
                _, site, src_t, mod, ordinal = r
                recs += struct.pack("<BBHHH", src_t, 1, site, modules.index(mod) + 1, ordinal)
            else:
                _, site, src_t, seg, off = r
                recs += struct.pack("<BBHBBH", src_t, 0, site, seg, 0, off)
            struct.pack_into("<H", data, site, 0xFFFF)      # a chain of one
        flags = SEG_PRELOAD | (SEG_DATA if d["segments"][n] == "data" else 0)
        if n != d["autodata"]:
            flags |= SEG_MOVABLE
        if recs:
            flags |= SEG_RELOCS
        length = len(data)
        minalloc = length
        if n == d["autodata"]:
            minalloc = length + d["stack"] + d["heap"]
            if minalloc > 0x10000:
                sys.exit("gen_ne: the automatic data segment overflows 64 KiB")
        seg_entries.append(struct.pack("<HHHH", pos >> ALIGN_SHIFT, length & 0xFFFF, flags,
                                       minalloc & 0xFFFF))
        body += data
        pos += len(data)
        if recs:
            nrec = len(recs) // 8
            body += struct.pack("<H", nrec) + recs
            pos += 2 + len(recs)

    ne = bytearray(0x40)
    struct.pack_into("<2sBBHHI", ne, 0, b"NE", 5, 10, entry_off, len(entry_table), 0)
    progflags = 0x0002 | 0x0008 | 0x0020                 # multiple data, protected mode, 286
    appflags = 0x03                                       # uses the Windows API
    struct.pack_into("<BB", ne, 0x0C, progflags, appflags)
    struct.pack_into("<HHH", ne, 0x0E, d["autodata"], d["heap"], d["stack"])
    struct.pack_into("<HH", ne, 0x14, entry[1], entry[0])  # CS:IP
    struct.pack_into("<HH", ne, 0x18, 0, d["autodata"])    # SS:SP (SP 0: top of the segment)
    struct.pack_into("<HHH", ne, 0x1C, nseg, len(modules), len(nonres))
    struct.pack_into("<HHHHH", ne, 0x22, seg_table_off, res_table_off, resident_off,
                     modref_off, imported_off)
    struct.pack_into("<IHHH", ne, 0x2C, nonres_file_off, 0, ALIGN_SHIFT, 0)
    struct.pack_into("<BB", ne, 0x36, 2, 0)                # target OS: Windows
    struct.pack_into("<HHHH", ne, 0x38, 0, 0, 0, 0x030A)    # expects Windows 3.10

    blob = (mz_stub() + bytes(ne) + b"".join(seg_entries) + resource_table + resident
            + modref + imported + entry_table + nonres + bytes(body))
    open(out, "wb").write(blob)
    return blob


def dump(path):
    """An independent reading of an NE file, printed as plain lines."""
    b = open(path, "rb").read()
    if b[:2] != b"MZ":
        sys.exit("not an MZ file")
    (ne,) = struct.unpack_from("<I", b, 0x3C)
    if b[ne:ne + 2] != b"NE":
        sys.exit("not an NE file")
    h = lambda off, fmt: struct.unpack_from(fmt, b, ne + off)
    nseg, nmod = h(0x1C, "<H")[0], h(0x1E, "<H")[0]
    seg_t, _, res_t, mod_t, imp_t = h(0x22, "<HHHHH")
    shift = h(0x32, "<H")[0]
    ip, cs = h(0x14, "<HH")
    sp, ss = h(0x18, "<HH")
    name_len = b[ne + res_t]
    print(f"module {b[ne + res_t + 1:ne + res_t + 1 + name_len].decode()}")
    print(f"entry {cs}:{ip:04x} stack {ss}:{sp:04x} autodata {h(0x0E, '<H')[0]} "
          f"heap {h(0x10, '<H')[0]} stacksize {h(0x12, '<H')[0]}")
    mods = []
    for i in range(nmod):
        (o,) = struct.unpack_from("<H", b, ne + mod_t + 2 * i)
        ln = b[ne + imp_t + o]
        mods.append(b[ne + imp_t + o + 1:ne + imp_t + o + 1 + ln].decode())
    print("imports " + " ".join(mods))
    for i in range(nseg):
        sector, length, flags, minalloc = struct.unpack_from("<HHHH", b, ne + seg_t + 8 * i)
        length = length or 0x10000
        kind = "data" if flags & SEG_DATA else "code"
        print(f"segment {i + 1} {kind} file {sector << shift:#x} length {length} "
              f"minalloc {minalloc or 0x10000} flags {flags:#06x}")
        if flags & SEG_RELOCS:
            at = (sector << shift) + length
            (n,) = struct.unpack_from("<H", b, at)
            for j in range(n):
                src, fl, site, a, c = struct.unpack_from("<BBHHH", b, at + 2 + 8 * j)
                if fl & 3 == 1:
                    print(f"  reloc {site:04x} src {src} import {mods[a - 1]}.{c}")
                elif fl & 3 == 0:
                    print(f"  reloc {site:04x} src {src} internal {a & 0xFF}:{c:04x}")
                else:
                    print(f"  reloc {site:04x} src {src} flags {fl}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="a NASM source (or, with --dump, an NE file)")
    ap.add_argument("-o", "--output", help="the .exe to write")
    ap.add_argument("--dump", action="store_true", help="print an NE file's contents")
    a = ap.parse_args()
    if a.dump:
        dump(a.source)
        return 0
    if not a.output:
        ap.error("-o is required")
    build(a.source, a.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
