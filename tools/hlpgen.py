#!/usr/bin/env python3
"""Writes small WinHelp 3.1 (.HLP) files, so the reader has input that is not Microsoft's.

`userland/lib/uhlp.c` is tested against the real 3.11 help files on a machine that
has them, and those are never in the repository. This writes the same container --
the internal directory and its B+ trees, a compressed |TOPIC, |SYSTEM, |FONT,
|CONTEXT, |CTXOMAP, |KWBTREE/|KWDATA and a |Phrases table -- from a few lines of
Python, so a check can run anywhere and can KNOW what a topic holds.

    from hlpgen import Help, Topic
    h = Help("Title", fonts=[("Helv", 20, 0, 3, 0x000000)])
    h.add(Topic("Home", ids=["home"], paras=[[("text", "Hello "), ("link", "go", "jump", "other"),
                                              ("text", "this"), ("endlink",)]]))
    open("x.hlp", "wb").write(h.build())

A paragraph is a list of items: ("text", s), ("font", n), ("pic",) (a picture command with no bitmap), ("link", text, "jump"|"popup"|
"hidden-jump"|"hidden-popup", context-id), ("macro", text, macro-string), ("endlink",),
("br",), ("tab",). An item list is ONE paragraph; end it by starting the next list.

THE LAYOUT RULES the reader and a real file share, and a writer must follow:
  - a block of |TOPIC is 0x1000 bytes holding LZ77 data of at most 0x4000 bytes, and a
    record may cross a block only with `span=True`, which cuts one stream every SPAN_CHUNK bytes, each block full to its last byte as
    Microsoft's compiler writes them (zero padding would decompress into garbage between blocks) (a record then goes on in the next block's first bytes);
  - TOPICPOS of a topic is block * 0x8000 + the display text written in its block so far;
  - a record's `next` link only goes forward.
"""
import struct

DIR_PAGE = 512
TB = 0x1000
SPAN_CHUNK = 3630            # literals-only LZ77 of this many bytes fills a 0x1000 block exactly


def hlp_hash(s):
    """HASH of a context string: h = h * 43 + x, mod 2**32 (the algorithm WinHelp documents)."""
    h = 0
    for ch in s:
        c = ord(ch)
        if "A" <= ch <= "Z":
            x = c - ord("A") + 17
        elif "a" <= ch <= "z":
            x = c - ord("a") + 17
        elif "1" <= ch <= "9":
            x = c - ord("0")
        elif ch == "0":
            x = 10
        elif ch == ".":
            x = 12
        elif ch == "_":
            x = 13
        else:
            continue
        h = (h * 43 + x) & 0xFFFFFFFF
    return h


def lz77(data, literals=False):
    """Greedy LZ77 in WinHelp's shape: a flag byte per 8 items, bit set = a 2-byte match
    (length - 3 in the top 4 bits, distance - 1 in the low 12). `literals` writes no match at all, so the
    output is exactly len + ceil(len / 8) bytes."""
    out = bytearray()
    n = len(data)
    seen = {}
    items = []
    i = 0
    while i < n:
        best_len, best_dist = 0, 0
        if i + 3 <= n and not literals:
            for j in reversed(seen.get(bytes(data[i:i + 3]), ())[-32:]):
                if i - j > 4096:
                    break
                k = 3
                while k < 18 and i + k < n and data[j + k] == data[i + k]:
                    k += 1
                if k > best_len:
                    best_len, best_dist = k, i - j
        step = best_len if best_len >= 3 else 1
        if best_len >= 3:
            items.append((1, struct.pack("<H", (best_len - 3) << 12 | (best_dist - 1))))
        else:
            items.append((0, bytes([data[i]])))
        for q in range(i, min(i + step, n - 2)):
            seen.setdefault(bytes(data[q:q + 3]), []).append(q)
        i += step
    for g in range(0, len(items), 8):
        flag = 0
        body = b""
        for b, (m, payload) in enumerate(items[g:g + 8]):
            flag |= m << b
            body += payload
        out.append(flag)
        out += body
    return bytes(out)


def c_ushort(v):
    return bytes([v * 2]) if v < 0x80 else struct.pack("<H", v * 2 + 1)


def c_short(v):
    if -64 <= v < 64:
        return bytes([(v * 2 + 0x80) & 0xFF])
    return struct.pack("<H", (v * 2 + 0x8000) | 1)


def c_long(v):
    if -0x4000 <= v < 0x4000:
        return struct.pack("<H", v * 2 + 0x8000)
    return struct.pack("<I", ((v * 2 + 0x80000000) | 1) & 0xFFFFFFFF)


def btree(entries, page_size, structure, flags=0x0002):
    """A B+ tree of already-sorted (key bytes, value bytes) entries. Keys of a string tree
    carry their NUL. Returns the whole tree, header first."""
    leaves = []
    cur = []
    used = 8
    for e in entries:
        if used + len(e[0]) + len(e[1]) > page_size and cur:
            leaves.append(cur)
            cur, used = [], 8
        cur.append(e)
        used += len(e[0]) + len(e[1])
    leaves.append(cur)

    def page(kind_body, n, prev, nxt, body_start):
        unused = page_size - body_start - len(kind_body)
        if unused < 0:
            raise ValueError("an entry is larger than the page")
        return struct.pack("<HHH", unused, n, prev) + (struct.pack("<H", nxt) if nxt is not None else b"") + kind_body + b"\0" * unused

    pages = []
    for i, ents in enumerate(leaves):
        prev = i - 1 if i else 0xFFFF
        nxt = i + 1 if i + 1 < len(leaves) else 0xFFFF
        pages.append(page(b"".join(k + v for k, v in ents), len(ents), prev, nxt, 8))
    level_pages = list(range(len(leaves)))
    first_keys = [ents[0][0] for ents in leaves]
    levels = 1
    while len(level_pages) > 1:
        # an index page: the first child in the header, then (key, child) for the rest
        nxt_pages, nxt_keys = [], []
        i = 0
        while i < len(level_pages):
            body = b""
            n = 0
            first = level_pages[i]
            fk = first_keys[i]
            j = i + 1
            while j < len(level_pages):
                ent = first_keys[j] + struct.pack("<H", level_pages[j])
                if 6 + len(body) + len(ent) > page_size:
                    break
                body += ent
                n += 1
                j += 1
            if j == i + 1 and j < len(level_pages) and n == 0:
                raise ValueError("an index entry is larger than the page")
            pages.append(page(body, n, first, None, 6))
            nxt_pages.append(len(pages) - 1)
            nxt_keys.append(fk)
            i = j
        level_pages, first_keys = nxt_pages, nxt_keys
        levels += 1
    root = level_pages[0]
    head = struct.pack("<HHH", 0x293B, flags, page_size) + structure.ljust(16, b"\0")[:16]
    head += struct.pack("<HHHHHHI", 0, page_size, root, 0xFFFF, len(pages), levels, len(entries))
    assert len(head) == 38
    return head + b"".join(pages)


class Topic:
    def __init__(self, title, ids=(), keywords=(), paras=(), macros=(), number=None, table=None):
        self.title = title
        self.ids = list(ids)
        self.keywords = list(keywords)
        self.paras = [list(p) if isinstance(p, (list, tuple)) else p for p in paras]
        self.macros = list(macros)
        self.table = table          # None, or a list of rows of cells; a cell is an item list
        self.number = number


class Help:
    def __init__(self, title, copyright_=None, fonts=None, phrases=(), windows=(), macros=(), block_limit=3000,
                 map_ids=(), span=False):
        self.title = title
        self.copyright = copyright_
        self.fonts = fonts or [("Helv", 20, 0, 3, 0)]
        self.phrases = [p.encode("latin-1") for p in phrases]
        self.windows = list(windows)
        self.macros = list(macros)
        self.block_limit = block_limit
        self.span = span             # records straddle block boundaries, as Microsoft's compiler writes them
        self.map_ids = list(map_ids)     # (number, topic index)
        self.topics = []
        self.contents = 0

    def add(self, t):
        self.topics.append(t)
        return len(self.topics) - 1

    # -- text and commands --------------------------------------------------------------

    def _phrase(self, text):
        """Text with phrase codes for the longest phrase that fits at each position."""
        if not self.phrases:
            return text
        out = bytearray()
        i = 0
        while i < len(text):
            best = -1
            for k, p in enumerate(self.phrases):
                if text.startswith(p, i) and (best < 0 or len(p) > len(self.phrases[best])):
                    best = k
            if best < 0:
                out.append(text[i])
                i += 1
                continue
            end = i + len(self.phrases[best])
            code = 0x100 + 2 * best
            if end < len(text) and text[end] == 0x20:
                code |= 1
                end += 1
            out += bytes([code >> 8, code & 0xFF])
            i = end
        return bytes(out)

    def _paragraph(self, items):
        """(command bytes, text bytes, decompressed length) of one paragraph's item list."""
        cmds = bytearray()
        text = bytearray()
        pending = b""

        def emit(cmd):
            nonlocal pending
            text.extend(self._phrase(pending) + b"\0")
            cmds.extend(cmd)
            raw.append(pending)
            pending = b""

        raw = []
        for it in items:
            k = it[0]
            if k == "text":
                pending += it[1].encode("latin-1")
            elif k == "font":
                emit(b"\x80" + struct.pack("<H", it[1]))
            elif k == "br":
                emit(b"\x81")
            elif k == "tab":
                emit(b"\x83")
            elif k == "pic":
                # A picture by index (mode 0) that names no bitmap, laid out as in Windows 3.11's own files: the
                # command, the type 0x22, a size, a hotspot count, then the size's worth: mode and index. The
                # SIZE COUNTS THE BYTES AFTER THE HOTSPOT COUNT. Like every command it takes the text before it.
                emit(b"\x86\x22" + c_long(4) + c_ushort(1) + struct.pack("<HH", 0, 1))
            elif k == "endlink":
                emit(b"\x89")
            elif k == "link":
                _, label, kind, ctx = it
                op = {"popup": 0xE2, "jump": 0xE3, "hidden-popup": 0xE6, "hidden-jump": 0xE7}[kind]
                emit(bytes([op]) + struct.pack("<I", hlp_hash(ctx)))
                pending += label.encode("latin-1")
            elif k == "macro":
                _, label, mac = it
                m = mac.encode("latin-1") + b"\0"
                emit(b"\xC8" + struct.pack("<H", len(m)) + m)
                pending += label.encode("latin-1")
            else:
                raise ValueError(k)
        text.extend(self._phrase(pending) + b"\0")
        raw.append(pending)
        cmds.append(0xFF)
        full = sum(len(r) + 1 for r in raw)
        return bytes(cmds), bytes(text), full

    def _para_info(self, spec):
        """The fixed part of a paragraph: 4 unused bytes, the bit field and what it announces."""
        return b"\0" * 4 + struct.pack("<H", 0)

    def _display(self, items, prev, nxt):
        cmds, text, full = self._paragraph(items)
        ld1 = c_long(full) + c_ushort(full) + self._para_info(None) + cmds
        return self._record(0x20, ld1, text, full, prev, nxt), full

    def _table(self, rows, ri, prev, nxt):
        """One row of a table as a record: the column layout, the row's cells, the row's end.
        A table is as many records as it has rows, each repeating the layout."""
        ncol = max(len(r) for r in rows)
        cells = []
        texts = bytearray()
        full = 0
        for col in range(ncol):
            row = rows[ri]
            cmds, text, f = self._paragraph(row[col] if col < len(row) else [("text", "")])
            cells.append(struct.pack("<h", col) + b"\0" * 3 + self._para_info(None) + cmds)
            texts += text
            full += f
        cols = b"".join(struct.pack("<hh", 9000 // ncol, 100) for _ in range(ncol))
        ld1 = (c_long(full) + c_ushort(full) + bytes([ncol, 0]) + struct.pack("<h", 32767) + cols +
               b"".join(cells) + struct.pack("<h", -1))
        return self._record(0x23, ld1, bytes(texts), full, prev, nxt), full

    def _record(self, typ, ld1, text, full, prev, nxt):
        datalen = 0x15 + len(ld1)
        total = datalen + len(text)
        return struct.pack("<IIIIIB", total, full, prev, nxt, datalen, typ) + ld1 + text

    def _header(self, t, bwd, fwd, number, prev, nxt):
        """A topic header: browse links, number and the no-non-scrolling-region marks (28 bytes of
        LinkData1), then the title and the topic's own macros."""
        body = t.title.encode("latin-1") + b"\0"
        for m in t.macros:
            body += m.encode("latin-1") + b"\0"
        ld1 = struct.pack("<IIIIIII", 0, bwd, fwd, number, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
        return self._record(0x02, ld1, body, len(body), prev, nxt)

    # -- the file --------------------------------------------------------------------------

    def build(self):
        # A topic is a header and its paragraphs; records are laid out in blocks first (sizes
        # do not depend on the browse offsets), then written with the offsets filled in.
        recs = []                         # (topic, kind, builder)
        for ti, t in enumerate(self.topics):
            recs.append((ti, "hdr", None))
            for ri in range(len(t.table or [])):
                recs.append((ti, "tbl", (t.table, ri)))
            for p in t.paras:
                recs.append((ti, "para", p))
        # pass 1: sizes and layout
        sizes = []
        for ti, kind, arg in recs:
            if kind == "hdr":
                r = self._header(self.topics[ti], 0, 0, 0, 0, 0)
                sizes.append((len(r), 0))
            elif kind == "tbl":
                r, full = self._table(arg[0], arg[1], 0, 0)
                sizes.append((len(r), full))
            else:
                r, full = self._display(arg, 0, 0)
                sizes.append((len(r), full))
        pos_of = {}                       # record index -> (block, offset in block, TOPICPOS)
        topic_pos = {}
        if self.span:
            # One stream cut every SPAN_CHUNK bytes (a block's worth of literals), wherever that falls: a record that starts near the end of a
            # block goes on in the next one's first bytes. TOPICPOS counts the display text of the records that
            # START in a block (the reader's rule).
            blocks = []
            at, cur, offs = 0, None, 0
            for idx, (n, full) in enumerate(sizes):
                bi, off = divmod(at, SPAN_CHUNK)
                if bi != cur:
                    cur, offs = bi, 0
                pos_of[idx] = (bi, off, bi * 0x8000 + offs)
                if recs[idx][1] == "hdr":
                    topic_pos[recs[idx][0]] = bi * 0x8000 + offs
                offs += full
                at += n
            blocks = [None] * (-(-at // SPAN_CHUNK))
        else:
            blocks = [[]]
            blen = 0
            for idx, (n, _) in enumerate(sizes):
                if blen + n > self.block_limit and blocks[-1]:
                    blocks.append([])
                    blen = 0
                blocks[-1].append(idx)
                blen += n
            for bi, members in enumerate(blocks):
                off = 0
                offs = 0
                for idx in members:
                    pos_of[idx] = (bi, off, bi * 0x8000 + offs)
                    ti, kind, _ = recs[idx]
                    if kind == "hdr":
                        topic_pos[ti] = bi * 0x8000 + offs
                    off += sizes[idx][0]
                    offs += sizes[idx][1]

        def ref(idx):
            bi, off, _ = pos_of[idx]
            return 0x0C + bi * 0x4000 + off

        # pass 2: the records, with their links
        raw_blocks = [bytearray() for _ in blocks]
        stream = bytearray()
        for idx, (ti, kind, arg) in enumerate(recs):
            nxt = ref(idx + 1) if idx + 1 < len(recs) else 0xFFFFFFFF
            prv = ref(idx - 1) if idx else 0xFFFFFFFF
            if kind == "hdr":
                bwd = topic_pos[ti - 1] if ti > 0 else 0xFFFFFFFF
                fwd = topic_pos[ti + 1] if ti + 1 < len(self.topics) else 0xFFFFFFFF
                r = self._header(self.topics[ti], bwd, fwd, ti + 1 if self.topics[ti].number is None else self.topics[ti].number, prv, nxt)
            elif kind == "tbl":
                r, _ = self._table(arg[0], arg[1], prv, nxt)
            else:
                r, _ = self._display(arg, prv, nxt)
            assert len(r) == sizes[idx][0]
            if self.span:
                stream += r
            else:
                raw_blocks[pos_of[idx][0]] += r
        if self.span:
            for bi in range(len(raw_blocks)):
                raw_blocks[bi] += stream[bi * SPAN_CHUNK:(bi + 1) * SPAN_CHUNK]

        topic = b""
        for bi, rb in enumerate(raw_blocks):
            comp = lz77(bytes(rb), literals=self.span)
            if 12 + len(comp) > TB:
                raise ValueError("block %d does not fit in 0x1000 bytes compressed; lower block_limit" % bi)
            blk = struct.pack("<III", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF) + comp
            if bi + 1 < len(raw_blocks):
                blk = blk.ljust(TB, b"\0")
            topic += blk

        # |CONTEXT: every id's hash, sorted, to its topic's TOPICPOS
        ctx = {}
        for ti, t in enumerate(self.topics):
            for cid in t.ids:
                ctx[hlp_hash(cid)] = topic_pos[ti]
        context = btree([(struct.pack("<I", h), struct.pack("<I", o)) for h, o in sorted(ctx.items())],
                        128, b"L4")

        # |KWBTREE / |KWDATA
        kw = {}
        for ti, t in enumerate(self.topics):
            for w in t.keywords:
                kw.setdefault(w, []).append(topic_pos[ti])
        kwdata = b""
        kwents = []
        for w in sorted(kw, key=lambda s: s.lower()):
            kwents.append((w.encode("latin-1") + b"\0", struct.pack("<HI", len(kw[w]), len(kwdata))))
            kwdata += b"".join(struct.pack("<I", p) for p in kw[w])
        kwtree = btree(kwents, 128, b"z2") if kwents else None

        # |SYSTEM
        flags = 4
        system = struct.pack("<HHH", 0x036C, 21, 1) + b"\0\0\0\0" + struct.pack("<H", flags)
        assert len(system) == 12
        rec = lambda typ, data: struct.pack("<HH", typ, len(data)) + data      # noqa: E731
        system += rec(1, self.title.encode("latin-1") + b"\0")
        if self.copyright:
            system += rec(2, self.copyright.encode("latin-1") + b"\0")
        system += rec(3, struct.pack("<I", topic_pos[self.contents]))
        for m in self.macros:
            system += rec(4, m.encode("latin-1") + b"\0")
        for w in self.windows:
            wr = bytearray(90)
            fl = 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x100 | 0x200
            struct.pack_into("<H", wr, 0, fl)
            wr[10:10 + len(w["name"]) + 1] = w["name"].encode("latin-1") + b"\0"
            wr[19:19 + len(w["caption"]) + 1] = w["caption"].encode("latin-1") + b"\0"
            struct.pack_into("<HHHH", wr, 72, w["x"], w["y"], w["w"], w["h"])
            struct.pack_into("<II", wr, 82, w.get("sr", 0xFFFFFF), w.get("nsr", 0xE0E0E0))
            system += rec(6, bytes(wr))

        # |FONT: 20-byte face names, then 11-byte descriptors
        faces = []
        for f in self.fonts:
            if f[0] not in faces:
                faces.append(f[0])
        fnt = struct.pack("<HHHH", len(faces), len(self.fonts), 8, 8 + 20 * len(faces))
        fnt += b"".join(f.encode("latin-1").ljust(20, b"\0") for f in faces)
        for face, half, attr, fam, rgb in self.fonts:
            fnt += bytes([attr, half, fam]) + struct.pack("<H", faces.index(face))
            fnt += bytes([(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255, 255, 255])

        subs = [("|SYSTEM", system), ("|TOPIC", topic), ("|CONTEXT", context), ("|FONT", fnt)]
        if kwtree:
            subs += [("|KWBTREE", kwtree), ("|KWDATA", kwdata)]
        if self.map_ids:
            subs.append(("|CTXOMAP", struct.pack("<H", len(self.map_ids)) +
                         b"".join(struct.pack("<II", n, topic_pos[t]) for n, t in self.map_ids)))
        if self.phrases:
            offs = []
            at = 2 * len(self.phrases) + 2
            for p in self.phrases:
                offs.append(at)
                at += len(p)
            offs.append(at)
            body = b"".join(self.phrases)
            ph = struct.pack("<HHI", len(self.phrases), 0x0100, len(body)) + b"".join(struct.pack("<H", o) for o in offs)
            ph += lz77(body)
            subs.append(("|Phrases", ph))
        return self._file(subs)

    @staticmethod
    def _file(subs):
        out = bytearray(b"\0" * 16)
        offsets = {}
        for name, data in subs:
            offsets[name] = len(out)
            out += struct.pack("<IIB", len(data) + 9, len(data), 0) + data
        entries = sorted(((n.encode("latin-1") + b"\0", struct.pack("<I", o)) for n, o in offsets.items()))
        tree = btree(entries, DIR_PAGE, b"z4", 0x0402)
        dstart = len(out)
        out += struct.pack("<IIB", len(tree) + 9, len(tree), 0) + tree
        struct.pack_into("<IIII", out, 0, 0x00035F3F, dstart, 0xFFFFFFFF, len(out))
        return bytes(out)


def sample():
    """The file the host check and the GUI test both use: a contents page, a table, a pop-up,
    a macro link, keywords, a second window and enough text to need several blocks."""
    fonts = [("Helv", 20, 0, 3, 0x000000), ("Helv", 24, 1, 3, 0x000000), ("Courier New", 20, 0, 1, 0x000000),
             ("Helv", 20, 0x04, 3, 0x008000)]
    h = Help("Sample Help", "(c) toy-os", fonts=fonts,
             phrases=["the quick ", "brown fox", "jumps over ", "lazy dog"],
             windows=[{"name": "side", "caption": "Side", "x": 10, "y": 20, "w": 300, "h": 400}],
             macros=["CreateButton(\"b1\", \"&Home\", \"JumpContents()\")",
                     "CreateButton(\"gb\", \"&Glossary\", \"JI(`SAMPLE.HLP>glossary', `gloss')\")"],
             map_ids=[(100, 1)])
    h.add(Topic("Contents", ids=["contents", "idh_contents"], keywords=["Overview"], paras=[
        [("font", 1), ("text", "Sample Help"), ("font", 0)],
        [("text", "Welcome. See "), ("link", "Details", "jump", "details"), ("endlink",),
         ("text", " or the "), ("link", "glossary", "popup", "gloss"), ("endlink",), ("text", ".")],
        [("text", "the quick brown fox jumps over the lazy dog")],
    ]))
    h.add(Topic("Details", ids=["details"], keywords=["Overview", "Details"], macros=["SaveMark(\"seen\")"], paras=[
        [("font", 1), ("text", "Details"), ("font", 0)],
        [("text", "Name"), ("tab",), ("text", "Value"), ("br",), ("text", "second line")],
        [("font", 2), ("text", "code()"), ("font", 0), ("text", " then "),
         ("macro", "run it", "Test(1)"), ("endlink",)],
        [("link", "hidden", "hidden-jump", "contents"), ("text", "x"), ("endlink",)],
        [("pic",), ("tab",), ("text", "Press "), ("font", 2), ("text", "BACKSPACE"), ("font", 0), ("text", " to delete.")],
        [("text", "-2"), ("pic",), ("text", "-1 to 2"), ("pic",), ("text", "-1")],
    ], table=[[[("text", "A1")], [("text", "B1")]], [[("text", "A2")], [("font", 3), ("text", "B2")]]]))
    h.add(Topic("Glossary entry", ids=["gloss"], keywords=["Glossary"], paras=[[("text", "A popup's text.")]]))
    for i in range(24):
        h.add(Topic("Filler %d" % i, ids=["filler%d" % i], keywords=["Filler"] if i % 6 == 0 else [],
                    paras=[[("text", "Paragraph %d of the filler text, long enough that the block holds a few. " % i * 3)]]))
    return h


if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "sample.hlp"
    open(out, "wb").write(sample().build())
    print("wrote", out)
