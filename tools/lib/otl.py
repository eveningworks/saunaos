"""tools/lib/otl.py -- read a TrueType face's cmap, kern table, glyph names and
GSUB ligatures in pure Python, for tools that must not depend on the tree's C.

Two users: `hostchecks/otl_hostcheck.py`, whose ORACLES these are (the `kern`
table and `post` glyph names are data the C code under test never reads for
the same answer), and `gen/gen_emojidata.py`, which keeps an emoji sequence
only when the emoji face draws it as one glyph.

Small on purpose: cmap formats 4 and 12, a format-0 `kern` table, `post`
version 2 names, and GSUB lookup type 4 (ligatures, through Extension 7) of
one feature in one script's default LangSys. Not a shaper, and not for
untrusted fonts -- it reads the faces this repository ships.

    f = Font("data/fonts/fallback/twemoji.ttf")
    f.gid(0x1F468); f.glyph_name(5999); f.ligate([597, 17, 598], "ccmp")
"""
import struct


def _u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def _s16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


class Font:
    def __init__(self, path):
        b = self.b = open(path, "rb").read()
        self.tables = {}
        for i in range(_u16(b, 4)):
            tag = b[12 + 16 * i:16 + 16 * i].decode("latin1")
            self.tables[tag] = _u32(b, 20 + 16 * i)
        self._cmap = None
        self._names = None

    def num_glyphs(self):
        return _u16(self.b, self.tables["maxp"] + 4)

    # -- cmap ---------------------------------------------------------------
    def cmap(self):
        if self._cmap is not None:
            return self._cmap
        b, o = self.b, self.tables["cmap"]
        best = None
        for i in range(_u16(b, o + 2)):
            off = _u32(b, o + 8 + 8 * i)
            fmt = _u16(b, o + off)
            if fmt == 12 or (fmt == 4 and best is None):
                best = (fmt, o + off)
        m = {}
        fmt, s = best
        if fmt == 12:
            for i in range(_u32(b, s + 12)):
                a, z, g = _u32(b, s + 16 + 12 * i), _u32(b, s + 20 + 12 * i), _u32(b, s + 24 + 12 * i)
                for c in range(a, z + 1):
                    m[c] = g + c - a
        else:
            n = _u16(b, s + 6) // 2
            ends, starts = s + 14, s + 16 + 2 * n
            deltas, ros = starts + 2 * n, starts + 4 * n
            for i in range(n):
                e, st = _u16(b, ends + 2 * i), _u16(b, starts + 2 * i)
                d, ro = _s16(b, deltas + 2 * i), _u16(b, ros + 2 * i)
                for c in range(st, e + 1):
                    if c == 0xFFFF:
                        continue
                    if ro == 0:
                        g = (c + d) & 0xFFFF
                    else:
                        g = _u16(b, ros + 2 * i + ro + 2 * (c - st))
                        g = (g + d) & 0xFFFF if g else 0
                    if g:
                        m[c] = g
        self._cmap = m
        return m

    def gid(self, cp):
        return self.cmap().get(cp, 0)

    # -- kern (format 0) ----------------------------------------------------
    def kern_pairs(self):
        """{(left, right): units} from the first horizontal format-0 subtable."""
        o = self.tables.get("kern")
        if o is None or _u16(self.b, o) != 0:
            return {}
        b, p = self.b, o + 4
        for _ in range(_u16(b, o + 2)):
            length, cov = _u16(b, p + 2), _u16(b, p + 4)
            if (cov >> 8) == 0 and cov & 1 and not cov & 4:
                n = _u16(b, p + 6)
                return {(_u16(b, p + 14 + 6 * i), _u16(b, p + 16 + 6 * i)): _s16(b, p + 18 + 6 * i)
                        for i in range(n)}
            p += length
        return {}

    # -- post names ---------------------------------------------------------
    def glyph_name(self, g):
        if self._names is None:
            b, p = self.b, self.tables["post"]
            if _u32(b, p) != 0x20000:
                raise ValueError("post is not version 2: no glyph names")
            n = _u16(b, p + 32)
            idx = [_u16(b, p + 34 + 2 * i) for i in range(n)]
            o, names = p + 34 + 2 * n, []
            while len(names) < max([i - 257 for i in idx] + [0]):
                ln = b[o]
                names.append(b[o + 1:o + 1 + ln].decode("latin1"))
                o += 1 + ln
            self._names = (idx, names)
        idx, names = self._names
        i = idx[g]
        return f"<standard {i}>" if i < 258 else names[i - 258]

    # -- GSUB ligatures -----------------------------------------------------
    def _coverage(self, o, g):
        b, fmt = self.b, _u16(self.b, o)
        if fmt == 1:
            for i in range(_u16(b, o + 2)):
                if _u16(b, o + 4 + 2 * i) == g:
                    return i
        else:
            for i in range(_u16(b, o + 2)):
                a, z, si = _u16(b, o + 4 + 6 * i), _u16(b, o + 6 + 6 * i), _u16(b, o + 8 + 6 * i)
                if a <= g <= z:
                    return si + g - a
        return -1

    def _lookups(self, feature, script):
        b, t = self.b, self.tables.get("GSUB")
        if t is None:
            return []
        sl, fl, ll = t + _u16(b, t + 4), t + _u16(b, t + 6), t + _u16(b, t + 8)
        for want in (script, "DFLT", "latn"):
            for i in range(_u16(b, sl)):
                if b[sl + 2 + 6 * i:sl + 6 + 6 * i].decode() != want:
                    continue
                so = sl + _u16(b, sl + 6 + 6 * i)
                if not _u16(b, so):
                    break
                ls = so + _u16(b, so)
                for k in range(_u16(b, ls + 4)):
                    fi = _u16(b, ls + 6 + 2 * k)
                    if b[fl + 2 + 6 * fi:fl + 6 + 6 * fi].decode() == feature:
                        fo = fl + _u16(b, fl + 6 + 6 * fi)
                        return [ll + _u16(b, ll + 2 + 2 * _u16(b, fo + 4 + 2 * j)) for j in range(_u16(b, fo + 2))]
                return []
        return []

    def ligate(self, gids, feature, script="DFLT"):
        """The run after `feature`'s ligature lookups, OpenType's per-position rule."""
        b = self.b
        for lk in self._lookups(feature, script):
            subs = []
            for k in range(_u16(b, lk + 4)):
                st, typ = lk + _u16(b, lk + 6 + 2 * k), _u16(b, lk)
                if typ == 7:
                    typ, st = _u16(b, st + 2), st + _u32(b, st + 4)
                if typ == 4:
                    subs.append(st)
            out, i = [], 0
            while i < len(gids):
                used = 0
                for st in subs:
                    ci = self._coverage(st + _u16(b, st + 2), gids[i])
                    if ci < 0:
                        continue
                    ls = st + _u16(b, st + 6 + 2 * ci)
                    for k in range(_u16(b, ls)):
                        lg = ls + _u16(b, ls + 2 + 2 * k)
                        comp = _u16(b, lg + 2)
                        if gids[i + 1:i + comp] == [_u16(b, lg + 4 + 2 * j) for j in range(comp - 1)] \
                                and i + comp <= len(gids):
                            out.append(_u16(b, lg))
                            used = comp
                            break
                    if used:
                        break
                if not used:
                    out.append(gids[i])
                    used = 1
                i += used
            gids = out
        return gids
