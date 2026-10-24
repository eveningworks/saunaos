#!/usr/bin/env python3
"""COMPRESS.EXE's two formats, WRITTEN -- SZDD and KWAJ (methods 0-3).

The reader is userland/lib/umsexpand.c; this is the other half, written
separately so the two can check each other (tools/umsexpand_hostcheck.py),
and so a test can build a Windows setup disk out of its own files: no
Microsoft file is ever needed in the repo to test w16's installer.

KWAJ method 3 is LZ + Huffman: five canonical tables (match lengths after
a match or a full literal run, match lengths after a shorter run, literal
run lengths, offset high bits, literals), each sent as code lengths in the
cheapest of four encodings, then the tokens, bits from each byte's high
bit down. The stream has no end marker -- it ends where its bytes do -- so
a file written here always records its length (flag 0x01), and a reader
stops there whatever the last byte's padding decodes to.

    python3 tools/mscompress.py kwaj IN OUT [--method N] [--name NAME.EXT]
    python3 tools/mscompress.py szdd IN OUT
"""
import argparse
import heapq
import struct
import sys

SZDD_MAGIC = b"SZDD\x88\xf0\x27\x33"
KWAJ_MAGIC = b"KWAJ\x88\xf0\x27\xd1"
WINDOW = 4096
NSYMS = (16, 16, 32, 64, 256)   # MATCHLEN1, MATCHLEN2, LITLEN, OFFSET, LITERAL


def _matches(data, i, heads, prev, max_len, max_dist, chain=48):
    """The longest earlier match at i: (length, distance), length 0 when none."""
    if i + 3 > len(data):
        return 0, 0
    key = data[i:i + 3]
    best = (0, 0)
    j = heads.get(key, -1)
    steps = 0
    while j >= 0 and steps < chain:
        d = i - j
        if d > max_dist:
            break
        n = 0
        while n < max_len and i + n < len(data) and data[j + n] == data[i + n]:
            n += 1
        if n > best[0]:
            best = (n, d)
            if n == max_len:
                break
        j = prev.get(j, -1)
        steps += 1
    return best


def _insert(data, i, heads, prev):
    if i + 3 <= len(data):
        key = data[i:i + 3]
        if key in heads:
            prev[i] = heads[key]
        heads[key] = i


def _lz(data, max_len, max_dist):
    """Greedy LZ77: a list of ('lit', byte) and ('match', length, distance)."""
    out = []
    heads, prev = {}, {}
    i = 0
    while i < len(data):
        n, d = _matches(data, i, heads, prev, max_len, max_dist)
        if n >= 3:
            out.append(("match", n, d))
            for k in range(n):
                _insert(data, i + k, heads, prev)
            i += n
        else:
            out.append(("lit", data[i]))
            _insert(data, i, heads, prev)
            i += 1
    return out


# --- SZDD and KWAJ method 2: LZSS -------------------------------------------------------------

def _lzss(data):
    """The LZSS EXPAND reads: control bytes LOW bit first, a match naming a
    WINDOW POSITION (the window starts at 4096 - 16)."""
    out = bytearray()
    tokens = _lz(data, 18, WINDOW - 18)
    pos = WINDOW - 16
    k = 0
    while k < len(tokens):
        ctl_at = len(out)
        out.append(0)
        ctl = 0
        for bit in range(8):
            if k >= len(tokens):
                break
            t = tokens[k]
            k += 1
            if t[0] == "lit":
                ctl |= 1 << bit
                out.append(t[1])
                pos = (pos + 1) % WINDOW
            else:
                _, n, d = t
                src = (pos - d) % WINDOW
                out += bytes((src & 0xFF, (src >> 4 & 0xF0) | (n - 3)))
                pos = (pos + n) % WINDOW
        out[ctl_at] = ctl
    return bytes(out)


def szdd(data, missing=b"_"):
    return SZDD_MAGIC + b"A" + missing[:1] + struct.pack("<I", len(data)) + _lzss(data)


# --- KWAJ method 3: LZ + Huffman --------------------------------------------------------------

class _Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, n):
        for k in range(n - 1, -1, -1):
            self.acc = self.acc << 1 | (value >> k & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = self.n = 0

    def done(self):
        if self.n:
            self.out.append(self.acc << (8 - self.n))
            self.acc = self.n = 0
        return bytes(self.out)


def _lengths(freq, limit=15):
    """Huffman code lengths for `freq`, none longer than `limit`, the code
    complete (Kraft sum exactly 1) whenever any symbol is used."""
    used = [s for s, f in enumerate(freq) if f]
    lens = [0] * len(freq)
    if not used:
        return lens
    if len(used) == 1:   # one symbol still needs a complete code: give it a partner
        other = 0 if used[0] else 1
        lens[used[0]] = lens[other] = 1
        return lens
    heap = [(freq[s], s, [s]) for s in used]
    heapq.heapify(heap)
    while len(heap) > 1:
        fa, sa, a = heapq.heappop(heap)
        fb, sb, b = heapq.heappop(heap)
        for s in a + b:
            lens[s] += 1
        heapq.heappush(heap, (fa + fb, min(sa, sb), a + b))
    # Too deep: lengthen the shortest codes until the deepest fit (Kraft kept exact).
    while max(lens) > limit:
        deep = [s for s in used if lens[s] > limit]
        for s in deep:
            lens[s] = limit
        while sum(2.0 ** -lens[s] for s in used) > 1.0:
            s = max((s for s in used if lens[s] < limit), key=lambda s: (lens[s], -freq[s]))
            lens[s] += 1
        # fill any slack back into the shortest codes, keeping the code complete
        while sum(2.0 ** -lens[s] for s in used) < 1.0:
            s = min((s for s in used if lens[s] > 1 and
                     sum(2.0 ** -lens[t] for t in used) + 2.0 ** -lens[s] <= 1.0),
                    key=lambda s: lens[s], default=None)
            if s is None:
                break
            lens[s] -= 1
    return lens


def _codes(lens):
    """Canonical codes, shortest first and by symbol within a length."""
    codes = [0] * len(lens)
    code = 0
    for L in range(1, 16):
        for s, l in enumerate(lens):
            if l == L:
                codes[s] = code
                code += 1
        code <<= 1
    return codes


def _len_encodings(lens, nsyms):
    """Each of the four length encodings that can carry `lens`, as (type, writer)."""
    fixed = {16: 4, 32: 5, 64: 6, 256: 8}[nsyms]
    out = []
    if all(l == fixed for l in lens):
        out.append((0, lambda bw: None))

    def t1(bw):
        c = lens[0]
        bw.put(c, 4)
        for l in lens[1:]:
            if l == c:
                bw.put(0, 1)
            elif l == c + 1:
                bw.put(0b10, 2)
                c = l
            else:
                bw.put(0b11, 2)
                bw.put(l, 4)
                c = l

    def t2(bw):
        c = lens[0]
        bw.put(c, 4)
        for l in lens[1:]:
            if l - c in (-1, 0, 1):
                bw.put(l - c + 1, 2)
            else:
                bw.put(3, 2)
                bw.put(l, 4)
            c = l

    def t3(bw):
        for l in lens:
            bw.put(l, 4)

    out += [(1, t1), (2, t2), (3, t3)]
    return out


def _cost(writer):
    b = _Bits()
    writer(b)
    return len(b.out) * 8 + b.n


def _lzh(data, force_type=None):
    tokens = _lz(data, 17, WINDOW - 1)
    # Group literals into runs of up to 32; after a run shorter than 32 the next
    # length comes from MATCHLEN2, otherwise from MATCHLEN1.
    seq = []          # (table, symbol) and ('bits6', v)
    lit_run = False
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t[0] == "match":
            _, n, d = t
            seq.append((1 if lit_run else 0, n - 2))
            seq.append((3, d >> 6))
            seq.append(("bits6", d & 63))
            lit_run = False
            i += 1
        else:
            run = []
            while i < len(tokens) and tokens[i][0] == "lit" and len(run) < 32:
                run.append(tokens[i][1])
                i += 1
            seq.append((1 if lit_run else 0, 0))
            seq.append((2, len(run) - 1))
            for b in run:
                seq.append((4, b))
            lit_run = len(run) != 32
    freq = [[0] * n for n in NSYMS]
    for t, s in seq:
        if t != "bits6":
            freq[t][s] += 1
    lens = [_lengths(f) for f in freq]
    codes = [_codes(l) for l in lens]
    bw = _Bits()
    choices = []
    for t in range(5):
        opts = _len_encodings(lens[t], NSYMS[t])
        if force_type is not None:
            pick = [o for o in opts if o[0] == force_type] or [opts[-1]]
            choices.append(pick[0])
        else:
            choices.append(min(opts, key=lambda o: _cost(o[1])))
    for t in range(5):
        bw.put(choices[t][0], 4)
    bw.put(0, 4)            # the sixth type, for alignment
    for t in range(5):
        choices[t][1](bw)
    for t, s in seq:
        if t == "bits6":
            bw.put(s, 6)
        else:
            bw.put(codes[t][s], lens[t][s])
    return bw.done()


def kwaj(data, method=3, name=None, force_type=None, with_length=True):
    if method == 0:
        body = data
    elif method == 1:
        body = bytes(b ^ 0xFF for b in data)
    elif method == 2:
        body = _lzss(data)
    elif method == 3:
        body = _lzh(data, force_type)
    else:
        raise ValueError("KWAJ method %d is not written here" % method)
    flags = 0
    extra = b""
    if with_length:
        flags |= 0x01
        extra += struct.pack("<I", len(data))
    if name:
        base, _, ext = name.upper().partition(".")
        flags |= 0x08
        extra += base[:8].encode() + b"\0"
        if ext:
            flags |= 0x10
            extra += ext[:3].encode() + b"\0"
    head = KWAJ_MAGIC + struct.pack("<HHH", method, 14 + len(extra), flags) + extra
    return head + body


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("kwaj")
    k.add_argument("src")
    k.add_argument("dst")
    k.add_argument("--method", type=int, default=3)
    k.add_argument("--name")
    s = sub.add_parser("szdd")
    s.add_argument("src")
    s.add_argument("dst")
    args = ap.parse_args()
    with open(args.src, "rb") as f:
        data = f.read()
    out = kwaj(data, args.method, args.name) if args.cmd == "kwaj" else szdd(data, args.src[-1:].encode())
    with open(args.dst, "wb") as f:
        f.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
