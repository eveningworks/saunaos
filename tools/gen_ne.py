#!/usr/bin/env python3
"""tools/gen_ne.py -- build a Win16 (NE) executable from one NASM source.

w16 (docs/w16-design.md) needs test programs, and the project ships no
Microsoft binary and needs no foreign toolchain: a test program is a
NASM source using userland/w16/samples/ne16.inc, assembled once per
segment, and this script lays the result out as an NE file -- an MZ
stub, the NE header and its tables, the segments and their relocations.

    python3 tools/gen_ne.py userland/w16/samples/hello.asm -o hello.exe
    python3 tools/gen_ne.py --dump hello.exe      # print what it holds

RESOURCES come from directives too, so a test program stays one text file:

    ;w16: string 1 Hello from a string table      (RT_STRING, blocks of 16)
    ;w16: bitmap 1 8x4 000000 ff0000 ffffff        (RT_BITMAP: size, palette)
    ;w16: row 1 00112200                           (its rows, top first, one
                                                    hex digit a pixel)
    ;w16: dialog 100 0 0 120 60 0x80c00000 "Caption"   (RT_DIALOG: x y w h in
                                                    dialog units, its style)
    ;w16: control 100 EDIT 10 8 8 50 12 0x50810000 ""  (a control of dialog 100:
                                                    class, id, x y w h, style, text)
    ;w16: menu 200                                  (RT_MENU, then its parts,
    ;w16: popup 200 "&Game"                         a popup's items after it)
    ;w16: item 200 510 "&New\tF2"                   (an id; "checked" or
    ;w16: separator 200                             "grayed" may follow)
    ;w16: accel 210 0x71 300 virtkey                (RT_ACCELERATOR: a key --
                                                    with virtkey, shift, control,
                                                    alt as it needs -- and its id)
    ;w16: cursor 1 15 15                            (RT_GROUP_CURSOR + RT_CURSOR,
    ;w16: crow 1 ....~....##..oo                    32x32, the hotspot; its rows
                                                    top first: . clear, ~ invert,
                                                    # black, o white; short rows
                                                    and missing ones are clear)

A bitmap is written as Windows stored one: a packed DIB (BITMAPINFOHEADER,
the palette, the rows bottom-up and padded to 4 bytes), 4 bits a pixel for
up to 16 colours, else 8.

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
import shlex
import struct
import subprocess
import sys
import tempfile

REL_IMPORT, REL_INTERNAL, REL_ENTRY, REL_EXPORT, REL_IMPNAME = 1, 2, 3, 4, 5
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
         "description": "", "strings": {}, "bitmaps": {}, "dialogs": {}, "menus": {}, "accels": {},
         "cursors": {}}
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
        elif key == "string":
            sid, _, text = val.partition(" ")
            d["strings"][int(sid, 0)] = text
        elif key == "bitmap":
            bid, size, *pal = val.split()
            w, h = (int(v) for v in size.split("x"))
            d["bitmaps"][int(bid, 0)] = {"w": w, "h": h, "pal": [int(c, 16) for c in pal], "rows": []}
        elif key == "row":
            bid, row = val.split()
            d["bitmaps"][int(bid, 0)]["rows"].append([int(ch, 16) for ch in row])
        elif key == "dialog":
            did, x, y, w, h, style, caption = shlex.split(val)
            d["dialogs"][int(did, 0)] = {"rect": [int(v, 0) for v in (x, y, w, h)],
                                         "style": int(style, 0), "caption": caption, "controls": []}
        elif key == "control":
            did, cls, cid, x, y, w, h, style, text = shlex.split(val)
            d["dialogs"][int(did, 0)]["controls"].append(
                (cls.upper(), int(cid, 0), [int(v, 0) for v in (x, y, w, h)], int(style, 0), text))
        elif key == "cursor":
            cid, hx, hy = (int(v, 0) for v in val.split())
            d["cursors"][cid] = {"hot": (hx, hy), "rows": []}
        elif key == "crow":
            cid, _, row = val.partition(" ")
            d["cursors"][int(cid, 0)]["rows"].append(row.strip())
        elif key == "accel":
            aid, k, cmd, *flags = val.split()
            bits = {"virtkey": 0x01, "shift": 0x04, "control": 0x08, "alt": 0x10}
            d["accels"].setdefault(int(aid, 0), []).append(
                (sum(bits[f] for f in flags), int(k, 0), int(cmd, 0)))
        elif key in ("menu", "popup", "item", "separator"):
            parts = shlex.split(val)
            mid = int(parts[0], 0)
            if key == "menu":
                d["menus"][mid] = []
            elif key == "popup":
                d["menus"][mid].append(("popup", parts[1], []))
            elif key == "item":
                d["menus"][mid][-1][2].append((int(parts[1], 0), parts[2].replace("\\t", "\t"),
                                               parts[3:]))
            else:
                d["menus"][mid][-1][2].append((0, "", []))
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
        elif kind == REL_IMPNAME:
            site, src, ln = struct.unpack_from("<HBB", raw, i)
            i += 4
            mod = raw[i:i + ln].decode("ascii")
            i += ln
            nl = raw[i]
            name = raw[i + 1:i + 1 + nl].decode("ascii")
            i += 1 + nl
            relocs.append(("impname", site, src, mod.upper(), name))
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


def cursor_resource(c):
    """RT_CURSOR, 3.x's: the hotspot, then a 1-bit DIB twice as tall as the
    cursor -- its XOR rows, then its AND rows, each bottom-up."""
    rows = (c["rows"] + [""] * 32)[:32]
    def mask(bit):
        out = b""
        for row in reversed(rows):
            row = row.ljust(32, ".")
            v = 0
            for x in range(32):
                a_, x_ = {".": (1, 0), "~": (1, 1), "#": (0, 0), "o": (0, 1)}[row[x]]
                v = v << 1 | (a_ if bit == "and" else x_)
            out += struct.pack(">I", v)
        return out
    bih = struct.pack("<IiiHHIIiiII", 40, 32, 64, 1, 1, 0, 0, 0, 0, 2, 0)
    return struct.pack("<HH", *c["hot"]) + bih + struct.pack("<II", 0, 0xFFFFFF) + mask("xor") + mask("and")


def string_blocks(strings):
    """RT_STRING: string n lives in block n // 16 + 1, as 16 counted strings."""
    blocks = {}
    for sid, text in strings.items():
        blocks.setdefault(sid // 16 + 1, {})[sid % 16] = text.encode("latin-1")
    out = {}
    for bid, entries in blocks.items():
        data = b""
        for i in range(16):
            s = entries.get(i, b"")
            data += bytes([len(s)]) + s
        out[bid] = data
    return out


def packed_dib(bm):
    """A bitmap resource: BITMAPINFOHEADER, palette, rows bottom-up."""
    w, h, pal, rows = bm["w"], bm["h"], bm["pal"], bm["rows"]
    if len(rows) != h or any(len(r) != w for r in rows):
        sys.exit(f"gen_ne: a bitmap's rows do not match its {w}x{h}")
    bpp = 4 if len(pal) <= 16 else 8
    ncol = 1 << bpp
    stride = ((w * bpp + 31) // 32) * 4
    bits = b""
    for r in reversed(rows):
        line = bytearray(stride)
        for x, v in enumerate(r):
            if v >= len(pal):
                sys.exit("gen_ne: a bitmap row names a colour the palette lacks")
            if bpp == 4:
                line[x // 2] |= v << (4 if x % 2 == 0 else 0)
            else:
                line[x] = v
        bits += bytes(line)
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h, 1, bpp, 0, len(bits), 0, 0, ncol, 0)
    quads = b"".join(struct.pack("<BBBB", c & 0xFF, (c >> 8) & 0xFF, c >> 16, 0)
                     for c in pal + [0] * (ncol - len(pal)))
    return hdr + quads + bits


CLASSES = {"BUTTON": 0x80, "EDIT": 0x81, "STATIC": 0x82, "LISTBOX": 0x83, "SCROLLBAR": 0x84,
           "COMBOBOX": 0x85}


def dialog_template(dlg):
    """RT_DIALOG, 3.x's DLGTEMPLATE: the style, the count, the rectangle in
    dialog units, no menu or class, the caption; then each control's
    rectangle, id, style, class byte and text."""
    x, y, w, h = dlg["rect"]
    out = struct.pack("<IBhhhh", dlg["style"], len(dlg["controls"]), x, y, w, h)
    out += b"\0\0" + dlg["caption"].encode("latin-1") + b"\0"
    for cls, cid, (cx, cy, cw, ch), style, text in dlg["controls"]:
        if cls not in CLASSES:
            sys.exit(f"gen_ne: a dialog control's class {cls} is not one of {sorted(CLASSES)}")
        out += struct.pack("<hhhhHIB", cx, cy, cw, ch, cid, style, CLASSES[cls])
        out += text.encode("latin-1") + b"\0" + b"\0"
    return out


def menu_template(menu):
    """RT_MENU, 3.x's: a header, then each top-level popup (MF_POPUP) and
    its items, MF_END on the last of each list."""
    out = struct.pack("<HH", 0, 0)
    for i, (_, title, items) in enumerate(menu):
        out += struct.pack("<H", 0x10 | (0x80 if i == len(menu) - 1 else 0))
        out += title.encode("latin-1") + b"\0"
        for j, (iid, text, flags) in enumerate(items):
            f = (0x08 if "checked" in flags else 0) | (0x01 if "grayed" in flags else 0)
            f |= 0x80 if j == len(items) - 1 else 0
            out += struct.pack("<HH", f, iid) + text.encode("latin-1") + b"\0"
    return out


def mz_stub():
    """A DOS stub that prints a line and exits with 1."""
    code = bytes([0x0E, 0x1F,                    # push cs; pop ds
                  0xBA, 0x0E, 0x00,              # mov dx, msg (offset 0x0E)
                  0xB4, 0x09, 0xCD, 0x21,        # mov ah,9; int 21h
                  0xB8, 0x01, 0x4C, 0xCD, 0x21])  # mov ax,4C01h; int 21h
    code += b"Needs a Win16 host (SaunaOS w16).\r\n$"
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
            if r[0] in ("import", "impname") and r[3] not in modules:
                modules.append(r[3])

    # Tables after the 64-byte NE header, in the order Microsoft's
    # linker wrote them: segment, resource, resident names, module
    # references, imported names, entry; non-resident names last.
    seg_table_off = 0x40
    res_table_off = seg_table_off + 8 * nseg
    # Resources: (type, id, bytes), types in order. Their file offsets are
    # known only once the segments are laid out, so the table is built
    # with placeholders here and filled in below.
    resources = [(6, bid, data) for bid, data in sorted(string_blocks(d["strings"]).items())]
    resources += [(2, bid, packed_dib(bm)) for bid, bm in sorted(d["bitmaps"].items())]
    resources += [(4, mid, menu_template(m)) for mid, m in sorted(d["menus"].items())]
    resources += [(5, did, dialog_template(dl)) for did, dl in sorted(d["dialogs"].items())]
    for cid, c in sorted(d["cursors"].items()):
        data = cursor_resource(c)
        resources.append((1, cid, data))
        resources.append((12, cid, struct.pack("<HHH", 0, 2, 1) +
                          struct.pack("<HHHHIH", 32, 64, 1, 1, len(data), cid)))
    resources += [(9, aid, b"".join(struct.pack("<BHH", f | (0x80 if i == len(acc) - 1 else 0), k, c)
                                   for i, (f, k, c) in enumerate(acc)))
                  for aid, acc in sorted(d["accels"].items())]
    resources.sort(key=lambda r: (r[0], r[1]))
    def resource_table_with(offsets):
        t = struct.pack("<H", ALIGN_SHIFT)
        for rtype in sorted({r[0] for r in resources}):
            entries = [(i, r) for i, r in enumerate(resources) if r[0] == rtype]
            t += struct.pack("<HHI", 0x8000 | rtype, len(entries), 0)
            for i, (_, rid, data) in entries:
                units = (len(data) + (1 << ALIGN_SHIFT) - 1) >> ALIGN_SHIFT
                t += struct.pack("<HHHHHH", offsets[i] >> ALIGN_SHIFT, units, 0x0030,
                                 0x8000 | rid, 0, 0)
        return t + struct.pack("<H", 0) + b"\0"
    resource_table = resource_table_with([0] * len(resources)) if resources else \
        struct.pack("<H", ALIGN_SHIFT) + struct.pack("<H", 0)
    resident = pstr(d["module"].upper()) + struct.pack("<H", 0)
    for ordinal, name, _, _ in sorted(exports):
        resident += pstr(name) + struct.pack("<H", ordinal)
    resident += b"\0"
    imported = b"\0"            # offset 0 is an empty name, as linkers wrote it
    modref = b""
    for m in modules:
        modref += struct.pack("<H", len(imported))
        imported += pstr(m)
    # Imports BY NAME: the function's name in the same table, each once; the relocation's second
    # word is where it sits.
    name_off = {}
    for n in range(1, nseg + 1):
        for r in relocs[n]:
            if r[0] == "impname" and r[4] not in name_off:
                name_off[r[4]] = len(imported)
                imported += pstr(r[4])
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
            elif r[0] == "impname":
                _, site, src_t, mod, name = r
                recs += struct.pack("<BBHHH", src_t, 2, site, modules.index(mod) + 1, name_off[name])
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
        # The automatic data segment's minalloc is its data alone, as
        # Microsoft's linker wrote it: the loader adds the heap and stack.
        minalloc = length
        if n == d["autodata"] and length + d["stack"] + d["heap"] > 0x10000:
            sys.exit("gen_ne: the automatic data segment overflows 64 KiB")
        seg_entries.append(struct.pack("<HHHH", pos >> ALIGN_SHIFT, length & 0xFFFF, flags,
                                       minalloc & 0xFFFF))
        body += data
        pos += len(data)
        if recs:
            nrec = len(recs) // 8
            body += struct.pack("<H", nrec) + recs
            pos += 2 + len(recs)

    # Resource data after the segments, each aligned.
    res_offsets = []
    for _, _, data in resources:
        pad = (-pos) % (1 << ALIGN_SHIFT)
        body += bytes(pad)
        pos += pad
        res_offsets.append(pos)
        # Padded to its last unit: the table gives the length in units, and
        # a reader checks every one of them lies inside the file.
        data += bytes((-len(data)) % (1 << ALIGN_SHIFT))
        body += data
        pos += len(data)
    if resources:
        resource_table = resource_table_with(res_offsets)

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
    (nonres,) = struct.unpack_from("<I", b, ne + 0x2C)
    (nonres_len,) = struct.unpack_from("<H", b, ne + 0x20)
    if nonres and nonres_len and b[nonres]:
        print(f"description {b[nonres + 1:nonres + 1 + b[nonres]].decode('latin-1')}")
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
    res_off = h(0x24, "<H")[0]
    if res_off != res_t:
        t = ne + res_off
        (rshift,) = struct.unpack_from("<H", b, t)
        p = t + 2
        def rname(rel):
            ln = b[t + rel]
            return "".join(c.upper() if "a" <= c <= "z" else c
                           for c in b[t + rel + 1:t + rel + 1 + ln].decode("latin-1"))
        while True:
            rtype, count = struct.unpack_from("<HH", b, p)
            if rtype == 0:
                break
            p += 8
            for _ in range(count):
                off, ln, _, rid = struct.unpack_from("<HHHH", b, p)
                p += 12
                ts = str(rtype & 0x7FFF) if rtype & 0x8000 else rname(rtype)
                ids = str(rid & 0x7FFF) if rid & 0x8000 else rname(rid)
                print(f"resource {ts} {ids} file {off << rshift:#x} length {ln << rshift}")


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
