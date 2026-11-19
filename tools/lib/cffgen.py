"""tools/lib/cffgen.py -- write CFF-outline OpenType fonts (and a collection of
them) from a TrueType face's glyphs, for tests of a CFF reader.

`hostchecks/cff_hostcheck.py` is the user: the tree's `ttf_cff.c` and
FreeType each render the SAME bytes this writes, and ttf.c renders the
TrueType glyph the CFF one came from -- three readings that share no code.

What a font from here exercises, on purpose:

  * BOTH KINDS OF CFF: name-keyed (one Private DICT) and CID-keyed (an
    FDArray of two font DICTs, FDSelect format 0 or 3, each with its own
    local subroutines), and `ttcf`, a collection holding one of each.
  * EVERY Type 2 path operator: the TrueType glyphs, their quadratics
    raised to cubics exactly, use rmoveto/hmoveto/vmoveto, rlineto,
    hlineto/vlineto runs and rrcurveto; SYNTHETIC glyphs (Private Use
    code points from U+E000) each run one of the rest -- hhcurveto,
    vvcurveto, hvcurveto/vhcurveto chains with a final df, rcurveline,
    rlinecurve, flex, hflex, hflex1 and flex1 both ways.
  * Operand encodings 32-246, 247-254, 28 and 255 (16.16), a width before
    the first operator, stems and hintmask/cntrmask bytes, local and
    global subroutines (nested, and with 1300 local ones, so the 1131
    bias is the one in use).

Not a general font compiler: fonts it writes are for the checks, built from
the faces this repository ships.

    font = cffgen.build("data/fonts/liberation-sans.ttf", "ABCabc", cid=True)
    open(path, "wb").write(font.otf)    # font.glyphs: [(cp, gid, ttf_gid)]
"""
import struct

import otl

PUA = 0xE000


# --- reading the TrueType face ---------------------------------------------

def _u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def _s16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


class Source:
    """The glyf outlines, advances and vertical metrics of a TrueType face."""

    def __init__(self, path):
        self.f = otl.Font(path)
        self.b = self.f.b
        t = self.f.tables
        self.upm = _u16(self.b, t["head"] + 18)
        self.loca_long = _s16(self.b, t["head"] + 50)
        self.ascent = _s16(self.b, t["hhea"] + 4)
        self.descent = _s16(self.b, t["hhea"] + 6)
        self.nhm = _u16(self.b, t["hhea"] + 34)

    def advance(self, g):
        return _u16(self.b, self.f.tables["hmtx"] + 4 * min(g, self.nhm - 1))

    def _range(self, g):
        lo = self.f.tables["loca"]
        if self.loca_long:
            return _u32(self.b, lo + 4 * g), _u32(self.b, lo + 4 * g + 4)
        return 2 * _u16(self.b, lo + 2 * g), 2 * _u16(self.b, lo + 2 * g + 2)

    def contours(self, g, dx=0, dy=0, depth=0):
        """[[(x, y, on), ...], ...], composites resolved (translations only)."""
        a, z = self._range(g)
        if a == z:
            return []
        b, o = self.b, self.f.tables["glyf"] + a
        n = _s16(b, o)
        if n < 0:
            out, p = [], o + 10
            while True:
                flags, comp = _u16(b, p), _u16(b, p + 2)
                p += 4
                if flags & 1:
                    x, y = _s16(b, p), _s16(b, p + 2)
                    p += 4
                else:
                    x, y = struct.unpack_from(">bb", b, p)
                    p += 2
                p += 2 if flags & 8 else 4 if flags & 0x40 else 8 if flags & 0x80 else 0
                out += self.contours(comp, dx + x, dy + y, depth + 1)
                if not flags & 0x20:
                    return out
        ends = [_u16(b, o + 10 + 2 * i) for i in range(n)]
        p = o + 10 + 2 * n
        p += 2 + _u16(b, p)
        npts = ends[-1] + 1 if ends else 0
        flags = []
        while len(flags) < npts:
            fl = b[p]
            p += 1
            flags.append(fl)
            if fl & 8:
                flags += [fl] * b[p]
                p += 1
        xs, ys, v = [], [], 0
        for fl in flags:
            if fl & 2:
                v += b[p] if fl & 0x10 else -b[p]
                p += 1
            elif not fl & 0x10:
                v += _s16(b, p)
                p += 2
            xs.append(v)
        v = 0
        for fl in flags:
            if fl & 4:
                v += b[p] if fl & 0x20 else -b[p]
                p += 1
            elif not fl & 0x20:
                v += _s16(b, p)
                p += 2
            ys.append(v)
        out, s = [], 0
        for e in ends:
            out.append([(xs[i] + dx, ys[i] + dy, flags[i] & 1) for i in range(s, e + 1)])
            s = e + 1
        return out


def segments(contour):
    """A TrueType contour as a start point and ('L', x, y) / ('C', 6 coords)
    segments, its quadratics raised to cubics (rounded to whole units)."""
    pts = contour
    n = len(pts)
    if n < 2:
        return None, []
    k = next((i for i in range(n) if pts[i][2]), None)
    if k is None:   # all off-curve: start at the first implied midpoint
        x0, y0 = (pts[-1][0] + pts[0][0]) / 2, (pts[-1][1] + pts[0][1]) / 2
        seq = pts
    else:
        x0, y0 = pts[k][0], pts[k][1]
        seq = pts[k + 1:] + pts[:k + 1]
    segs, cx, cy, i = [], x0, y0, 0
    while i < len(seq):
        x, y, on = seq[i]
        if on:
            segs.append(("L", x, y))
            cx, cy = x, y
            i += 1
            continue
        nx, ny, non = seq[(i + 1) % len(seq)] if i + 1 < len(seq) else (x0, y0, 1)
        if non:
            ex, ey, i = nx, ny, i + 2
        else:
            ex, ey, i = (x + nx) / 2, (y + ny) / 2, i + 1
        segs.append(("C", cx + 2 * (x - cx) / 3, cy + 2 * (y - cy) / 3,
                     ex + 2 * (x - ex) / 3, ey + 2 * (y - ey) / 3, ex, ey))
        cx, cy = ex, ey
    segs = [(s[0],) + tuple(round(v) for v in s[1:]) for s in segs]
    return (round(x0), round(y0)), segs


# --- Type 2 charstrings ------------------------------------------------------

def num(v, fixed=False):
    """One charstring operand: the shortest integer form, or 255 (16.16)."""
    if fixed or v != int(v):
        return bytes([255]) + struct.pack(">i", round(v * 65536))
    v = int(v)
    if -107 <= v <= 107:
        return bytes([v + 139])
    if 108 <= v <= 1131:
        v -= 108
        return bytes([(v >> 8) + 247, v & 0xFF])
    if -1131 <= v <= -108:
        v = -v - 108
        return bytes([(v >> 8) + 251, v & 0xFF])
    return bytes([28]) + struct.pack(">h", v)


OPS = {"hstem": [1], "vstem": [3], "vmoveto": [4], "rlineto": [5], "hlineto": [6], "vlineto": [7],
       "rrcurveto": [8], "callsubr": [10], "return": [11], "endchar": [14], "hstemhm": [18],
       "hintmask": [19], "cntrmask": [20], "rmoveto": [21], "hmoveto": [22], "vstemhm": [23],
       "rcurveline": [24], "rlinecurve": [25], "vvcurveto": [26], "hhcurveto": [27],
       "callgsubr": [29], "vhcurveto": [30], "hvcurveto": [31],
       "hflex": [12, 34], "flex": [12, 35], "hflex1": [12, 36], "flex1": [12, 37]}


def cs(*items):
    """Operands and operator names to bytes; bytes pass through (a mask)."""
    out = b""
    for it in items:
        if isinstance(it, str):
            out += bytes(OPS[it])
        elif isinstance(it, bytes):
            out += it
        elif isinstance(it, tuple):   # ("fixed", v)
            out += num(it[1], fixed=True)
        else:
            out += num(it)
    return out


def glyph_program(start, segs_list, width, style):
    """A TrueType glyph's charstring. `style` picks the width, the hints and
    how runs of lines are written, so the glyphs between them cover each."""
    out = b""
    if style % 2:
        out += cs(width)
    if style % 3 == 1:
        out += cs(10, 40, 600, 30, "hstemhm", 20, 50, "vstemhm")
        out += cs("hintmask", b"\xF0")          # 4 stems: one byte of mask
    x, y = 0, 0
    for (sx, sy), segs in zip(start, segs_list):
        dx, dy = sx - x, sy - y
        out += cs(dx, "hmoveto") if dy == 0 and style % 4 == 2 else \
            cs(dy, "vmoveto") if dx == 0 and style % 4 == 2 else cs(dx, dy, "rmoveto")
        x, y = sx, sy
        i = 0
        while i < len(segs):
            s = segs[i]
            if s[0] == "C":
                args = []
                while i < len(segs) and segs[i][0] == "C" and len(args) < 42:
                    _, ax, ay, bx, by, ex, ey = segs[i]
                    args += [ax - x, ay - y, bx - ax, by - ay, ex - bx, ey - by]
                    x, y = ex, ey
                    i += 1
                out += cs(*args, "rrcurveto")
                continue
            # A run of lines: alternating horizontal/vertical ones as
            # hlineto/vlineto, anything else as rlineto.
            run = []
            while i < len(segs) and segs[i][0] == "L" and len(run) < 40:
                run.append((segs[i][1] - x, segs[i][2] - y))
                x, y = segs[i][1], segs[i][2]
                i += 1
            j = 0
            while j < len(run):
                h0 = run[j][1] == 0 and run[j][0] != 0
                v0 = run[j][0] == 0 and run[j][1] != 0
                if style % 2 == 0 and (h0 or v0):
                    vals, horiz, k = [], h0, j
                    while k < len(run) and ((horiz and run[k][1] == 0) or (not horiz and run[k][0] == 0)):
                        vals.append(run[k][0] if horiz else run[k][1])
                        horiz = not horiz
                        k += 1
                    out += cs(*vals, "hlineto" if h0 else "vlineto")
                    j = k
                else:
                    out += cs(run[j][0], run[j][1], "rlineto")
                    j += 1
    return out + cs("endchar")


# Synthetic glyphs, one per operator the TrueType ones never write. Each is a
# list of contours, each contour its charstring after the moveto (relative
# throughout); all stay inside the em so both renderers draw them whole.
SYNTH = [
    ("hhcurveto", b"", (200, 200), cs(80, 500, 200, 300, 200, "hhcurveto", 0, 400, "rlineto")),
    ("vvcurveto", b"", (300, 100), cs(-60, 400, 200, 100, 300, "vvcurveto", 400, 0, "rlineto")),
    ("hvcurveto chain", b"", (200, 300), cs(300, 200, 300, 300, 300, -200, -300, -300, 200, -300, -200, -300,
                                           "hvcurveto")),
    ("vhcurveto + df", b"", (500, 100), cs(300, 300, 200, 300, 300, -300, 200, -300, 40, "vhcurveto")),
    ("rcurveline", b"", (200, 200), cs(100, 300, 200, 200, 300, 0, 200, -100, 200, -300, 100, -200,
                                       -200, -200, "rcurveline")),
    ("rlinecurve", b"", (200, 200), cs(600, 0, 0, 500, -100, 200, -300, 100, -200, -200, "rlinecurve")),
    ("flex", b"", (150, 300), cs(100, 200, 150, 50, 150, 0, 150, 0, 150, -50, 100, -200, 50, "flex",
                                 0, 600, -800, 0, "rlineto")),
    ("hflex", b"", (150, 400), cs(150, 150, 250, 150, 150, 150, 150, "hflex", 0, 500, -900, 0, "rlineto")),
    ("hflex1", b"", (150, 400), cs(150, 150, 150, 120, 150, 150, 150, -100, 150, "hflex1",
                                   0, 500, -900, 0, "rlineto")),
    ("flex1 wide", b"", (150, 400), cs(150, 100, 150, 100, 150, 0, 150, 0, 150, -100, 150, "flex1",
                                       0, 500, -900, 0, "rlineto")),
    ("flex1 tall", b"", (300, 100), cs(100, 150, 100, 150, 0, 150, 0, 150, -100, 150, 150, "flex1",
                                       600, 0, 0, -750, "rlineto")),
    ("fixed and 28 operands", b"", (("fixed", 200.5), 150), cs(("fixed", 500.25), 0, 0, 1200, -500, 0,
                                                                "rlineto")),
    ("stems and cntrmask", cs(10, 20, 30, 40, "hstem", 10, 20, "vstem", "cntrmask", b"\xE0"), (200, 200),
     cs(500, 0, 0, 500, -500, 0, "rlineto")),
]


def synth_program(pre, start, body, style):
    """A synthetic glyph: a width on half of them, `pre` (hints), one contour."""
    if style % 2:
        pre = cs(600) + pre if pre else cs(600)
    return pre + cs(start[0], start[1], "rmoveto") + body + cs("endchar")


# --- the CFF table -------------------------------------------------------------

def index(items):
    if not items:
        return b"\0\0"
    offs, o = [1], 1
    for it in items:
        o += len(it)
        offs.append(o)
    osz = 1 if o < 0x100 else 2 if o < 0x10000 else 3 if o < 0x1000000 else 4
    out = struct.pack(">HB", len(items), osz)
    for v in offs:
        out += v.to_bytes(osz, "big")
    return out + b"".join(items)


def dnum(v):
    """A DICT operand as a 5-byte integer: its size never depends on its value."""
    return bytes([29]) + struct.pack(">i", v)


def dop(*codes):
    return bytes(codes)


def subroutinize(progs, local_n=1300):
    """Moves each program's body after its first moveto into a subroutine
    -- local for some, global for others, a global one calling a local one
    for a few -- and pads the local INDEX to `local_n` items so the 1131
    bias applies. Returns (programs, local subrs, global subrs)."""
    lbias = 107 if local_n < 1240 else 1131 if local_n < 33900 else 32768
    local = [cs("return") for _ in range(local_n)]
    glob = []
    out = []
    for i, p in enumerate(progs):
        if i % 3 == 0 or len(p) < 8:
            out.append(p)
            continue
        cut = len(p) - 1                   # everything but endchar
        body = p[:cut]
        if i % 3 == 1:
            k = 2 * i % local_n
            local[k] = body + cs("return")
            out.append(cs(k - lbias, "callsubr", "endchar"))
        else:
            gk = len(glob)
            k = (2 * i + 1) % local_n
            local[k] = body + cs("return")
            glob.append(cs(k - lbias, "callsubr", "return"))
            out.append(cs(gk - 107, "callgsubr", "endchar"))
    return out, local, glob


def cff_table(progs, cid, name=b"SaunaTestCFF"):
    """The `CFF ` table: name-keyed with one Private DICT, or CID-keyed with
    two font DICTs (even glyphs in one, odd in the other)."""
    ng = len(progs)
    if cid:
        # Each FD gets its own local subroutines: split the programs by fd.
        fds = [[], []]
        for g, p in enumerate(progs):
            fds[g % 2].append(g)
        locals_, globs, outp = [], [], [None] * ng
        gbase = 0
        for fd, gids in enumerate(fds):
            sub, local, glob = subroutinize([progs[g] for g in gids],
                                            local_n=1300 if fd == 0 else 2 * len(gids) + 2)
            # Global subrs are shared: renumber this fd's calls past the last fd's.
            fixed = []
            for p in sub:
                if p.endswith(cs("callgsubr", "endchar")):
                    gk = _decode_first(p) + 107 + gbase
                    p = cs(gk - 107, "callgsubr", "endchar")
                fixed.append(p)
            gbase += len(glob)
            globs += glob
            locals_.append(local)
            for g, p in zip(gids, fixed):
                outp[g] = p
        progs, glob = outp, globs
    else:
        progs, local, glob = subroutinize(progs)
    strings = [b"Sauna", b"Test"]

    def layout(offs):
        hdr = bytes([1, 0, 4, 4])
        top = dnum(offs.get("chars", 0)) + dop(17)
        if cid:
            top = dnum(391) + dnum(392) + dnum(0) + dop(12, 30) + dnum(ng) + dop(12, 34) + top
            top += dnum(offs.get("charset", 0)) + dop(15)
            top += dnum(offs.get("fdarray", 0)) + dop(12, 36) + dnum(offs.get("fdselect", 0)) + dop(12, 37)
        else:
            top += dnum(offs.get("priv_size", 0)) + dnum(offs.get("priv", 0)) + dop(18)
        head = hdr + index([name]) + index([top]) + index(strings) + index(glob)
        body = {}
        o = len(head)
        body["chars"] = (o, index(progs))
        o += len(body["chars"][1])
        if cid:
            charset = bytes([0]) + b"".join(struct.pack(">H", g) for g in range(1, ng))
            body["charset"] = (o, charset)
            o += len(charset)
            # FDSelect format 3: alternating ranges of one glyph each.
            fs = bytes([3]) + struct.pack(">H", ng)
            for g in range(ng):
                fs += struct.pack(">HB", g, g % 2)
            fs += struct.pack(">H", ng)
            body["fdselect"] = (o, fs)
            o += len(fs)
            fdicts, privs = [], []
            for fd in range(2):
                pv = dnum(0) + dop(20) + dnum(0) + dop(21) + dnum(offs.get(f"subrs{fd}", 0)) + dop(19)
                privs.append(pv)
                fdicts.append(dnum(len(pv)) + dnum(offs.get(f"priv{fd}", 0)) + dop(18))
            fa = index(fdicts)
            body["fdarray"] = (o, fa)
            o += len(fa)
            for fd in range(2):
                body[f"priv{fd}"] = (o, privs[fd])
                o += len(privs[fd])
                li = index(locals_[fd])
                body[f"subrs{fd}"] = (o, li)
                o += len(li)
        else:
            pv = dnum(0) + dop(20) + dnum(0) + dop(21) + dnum(offs.get("subrs_rel", 0)) + dop(19)
            body["priv"] = (o, pv)
            body["priv_size"] = (len(pv), b"")
            o += len(pv)
            li = index(local)
            body["subrs"] = (o, li)
            o += len(li)
        return head, body

    offs = {}
    for _ in range(3):   # fixed-size operands: settles on the second pass
        head, body = layout(offs)
        offs = {k: v[0] for k, v in body.items()}
        if cid:
            for fd in range(2):
                offs[f"subrs{fd}"] = body[f"subrs{fd}"][0] - body[f"priv{fd}"][0]
        else:
            offs["subrs_rel"] = body["subrs"][0] - body["priv"][0]
    head, body = layout(offs)
    out = bytearray(head)
    for k, (o, blob) in sorted(body.items(), key=lambda kv: kv[1][0]):
        if not blob:
            continue
        assert o == len(out), (k, o, len(out))
        out += blob
    return bytes(out)


def _decode_first(p):
    """The first operand of a program written by num()."""
    b0 = p[0]
    if 32 <= b0 <= 246:
        return b0 - 139
    if 247 <= b0 <= 250:
        return (b0 - 247) * 256 + p[1] + 108
    if 251 <= b0 <= 254:
        return -(b0 - 251) * 256 - p[1] - 108
    if b0 == 28:
        return struct.unpack(">h", p[1:3])[0]
    raise ValueError("not an integer operand")


# --- the OpenType wrapper -------------------------------------------------------

def cmap_table(pairs):
    """Format 4, one segment per code point."""
    pairs = sorted(pairs) + [(0xFFFF, 0)]
    n = len(pairs)
    ends = b"".join(struct.pack(">H", c) for c, _ in pairs)
    starts = b"".join(struct.pack(">H", c) for c, _ in pairs)
    deltas = b"".join(struct.pack(">h", ((g - c) + 0x8000) % 0x10000 - 0x8000) if c != 0xFFFF
                      else struct.pack(">h", 1) for c, g in pairs)
    ros = b"\0\0" * n
    sub = struct.pack(">HHHHHHH", 4, 0, 0, 2 * n, 0, 0, 0) + ends + b"\0\0" + starts + deltas + ros
    sub = sub[:2] + struct.pack(">H", len(sub)) + sub[4:]
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + sub


def sfnt(tables, tag=b"OTTO"):
    """A font's bytes: (tag, data) tables after their directory (sorted by
    tag, as the format requires), 4-aligned. The `CFF ` table's bytes go
    LAST, so a reader that runs off the table runs off the file, where a
    sanitizer sees it."""
    tables = sorted(tables)
    n = len(tables)
    hdr = tag + struct.pack(">HHHH", n, 0, 0, 0)
    o = 12 + 16 * n
    at, blobs = {}, b""
    for t, d in sorted(tables, key=lambda td: td[0] == b"CFF "):
        at[t] = o + len(blobs)
        blobs += d + (b"\0" * (-len(d) % 4) if t != b"CFF " else b"")
    dirs = b"".join(t + struct.pack(">III", 0, at[t], len(d)) for t, d in tables)
    return hdr + dirs + blobs


def collection(fonts):
    """A `ttcf` of whole fonts: each member's table offsets moved to where
    its bytes now sit. The last member is not padded, so its last table
    still ends the file."""
    n = len(fonts)
    o = 12 + 4 * n
    head = b"ttcf" + struct.pack(">II", 0x00010000, n)
    body = b""
    for k, font in enumerate(fonts):
        base = o + len(body)
        head += struct.pack(">I", base)
        f = bytearray(font)
        for i in range(_u16(f, 4)):
            r = 12 + 16 * i
            struct.pack_into(">I", f, r + 8, _u32(f, r + 8) + base)
        body += bytes(f) + (b"" if k == n - 1 else b"\0" * (-len(f) % 4))
    return head + body


class Built:
    def __init__(self, otf, glyphs, synth):
        self.otf = otf
        self.glyphs = glyphs   # [(code point, gid here, gid in the TrueType face or 0)]
        self.synth = synth     # [(code point, what it exercises)]


def build(src_path, text, cid=False, name=b"SaunaTestCFF"):
    """A CFF OpenType font of `text`'s characters from the TrueType face at
    `src_path`, plus the synthetic glyphs at U+E000 upward."""
    src = Source(src_path)
    progs = [cs("endchar")]                       # .notdef: empty
    adv = [src.advance(0)]
    glyphs, cmap = [], []
    for k, ch in enumerate(dict.fromkeys(text)):
        tg = src.f.gid(ord(ch))
        if not tg:
            continue
        starts, segs = [], []
        for c in src.contours(tg):
            s, sg = segments(c)
            if s is not None:
                starts.append(s)
                segs.append(sg)
        gid = len(progs)
        progs.append(glyph_program(starts, segs, src.advance(tg), k))
        adv.append(src.advance(tg))
        glyphs.append((ord(ch), gid, tg))
        cmap.append((ord(ch), gid))
    synth = []
    for k, (what, pre, start, body) in enumerate(SYNTH):
        gid = len(progs)
        progs.append(synth_program(pre, start, body, k))
        adv.append(1200)
        cp = PUA + k
        glyphs.append((cp, gid, 0))
        cmap.append((cp, gid))
        synth.append((cp, what))
    ng = len(progs)
    head = bytearray(54)
    struct.pack_into(">IIII", head, 0, 0x00010000, 0x00010000, 0, 0x5F0F3CF5)
    struct.pack_into(">HH", head, 16, 0, src.upm)
    struct.pack_into(">hhhh", head, 36, -1000, src.descent, 3000, src.ascent)
    struct.pack_into(">hhh", head, 48, 2, 0, 0)
    hhea = bytearray(36)
    struct.pack_into(">Ihhh", hhea, 0, 0x00010000, src.ascent, src.descent, 0)
    struct.pack_into(">H", hhea, 10, max(adv))
    struct.pack_into(">H", hhea, 34, ng)
    hmtx = b"".join(struct.pack(">Hh", a, 0) for a in adv)
    maxp = struct.pack(">IH", 0x00005000, ng)
    tables = [(b"CFF ", cff_table(progs, cid, name)), (b"cmap", cmap_table(cmap)),
              (b"head", bytes(head)), (b"hhea", bytes(hhea)), (b"hmtx", hmtx), (b"maxp", maxp)]
    return Built(sfnt(tables), glyphs, synth)
