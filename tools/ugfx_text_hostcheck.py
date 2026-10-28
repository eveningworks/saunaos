#!/usr/bin/env python3
"""Check the toolkit's text measurement and uui_textbox's geometry on the HOST.

WHY A HOST HARNESS. What decides where a caret sits, which character a
click selects, and how far a field has scrolled is pure arithmetic over
per-glyph advances. It is also arithmetic that was WRONG in every widget
for as long as the interface face was monospace -- because on a
monospace face `n * char_w` and a real measurement are the same number,
so the bug was perfectly invisible until `system.font.face` became
Liberation Sans. A screenshot can show that a caret looks wrong; only a
sweep can show that it is right for every string, at every scroll
position, with kerning on.

So this compiles the real `userland/ui/ugfx_text.c` and
`userland/ui/uui_textbox.c` with the host gcc against a SYNTHETIC
proportional face, and checks three things:

**The measurement, against an oracle.** `ugfx_text_width_n()` and
`ugfx_text_index_at_x()` against a Python walk of the same advances.
The oracle shares no code with the implementation, which is what lets it
catch an EXPECTATION being wrong rather than only a transcription.

**The ROUND TRIP, which is the one that matters.** For every character
of every case, at every caret position: the x the widget DRAWS that
character at must hit-test back to that character's own index. This is
what catches draw() and index_at_x() drifting apart -- the failure that
had a click land on a different glyph than the one under the pointer,
and the reason both go through one `field_window_start()`.

**The window.** A value longer than its field must keep the caret
visible at every cursor position, must never scroll further than it has
to, and must show nothing past the field's inner edge.

**UTF-8 (docs/utf8-design.md).** Every case is BYTES, and several are
Polish, Greek, Cyrillic, box drawing, CJK, a Latin-1 word and broken
sequences. The oracle decodes them itself -- strict UTF-8, a stray byte
one character with its Latin-1 value -- so every index the C returns
must be a character boundary the oracle agrees on, and an advance past
U+00FF is its own width. The EDITING half drives uui_edit through a
textbox and reads back the buffer's bytes: one Right steps over a whole
`ż`, one Backspace removes both of its bytes, a typed U+017C inserts
C5 BC, and a masked `żż` shows two stars.

The synthetic face is deliberately HOSTILE: 'i' and 'l' are 3px where
'W' and 'M' are 20, so any surviving `* char_w` is off by up to 6x
rather than by a rounding error. Kerning is non-zero on real pairs
("AV", "To"), because a measurement that ignores it agrees with drawing
only when no such pair appears.

`--positive-control` reverts the geometry to the cell arithmetic this
replaced and requires the suite to go RED; `--positive-control-bytes`
puts back byte-at-a-time decoding and stepping (the code before UTF-8)
and requires the UTF-8 checks to go RED. A check that has never failed
is not evidence, and this one is entirely about a class of bug that
hides in plain sight.

Exit status is non-zero on any violation. Needs gcc.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The synthetic face. Everything not named here is MID wide.
NARROW = "iljt.,;:'|!"
WIDE = "WMmw@"
MID_W, NARROW_W, WIDE_W, CELL = 8, 3, 20, 20     # CELL = the WIDEST advance
FAR_W = 11     # any character past U+00FF: the glyph cache's, not the atlas's
KERNS = {("A", "V"): -2, ("V", "A"): -2, ("T", "o"): -3, ("o", "T"): -1}


def decode(b, i, end):
    """The oracle's decoder, written from RFC 3629 rather than from
    lib/uutf8.h: (codepoint, length) of the character at b[i], a byte
    that starts no valid sequence being ONE character of its own value."""
    c = b[i]
    if c < 0x80:
        return c, 1
    if 0xC2 <= c <= 0xDF:
        n, lo = 2, 0x80
    elif 0xE0 <= c <= 0xEF:
        n, lo = 3, 0x800
    elif 0xF0 <= c <= 0xF4:
        n, lo = 4, 0x10000
    else:
        return c, 1
    if i + n > end:
        return c, 1
    try:
        cp = ord(b[i:i + n].decode("utf-8"))
    except UnicodeDecodeError:
        return c, 1
    if cp < lo:
        return c, 1
    return cp, n


def chars(b, n=-1):
    """[(start, codepoint)] of the first n bytes, decoded as the C does."""
    end = len(b) if n < 0 else min(n, len(b))
    out, i = [], 0
    while i < end:
        cp, k = decode(b, i, end)
        out.append((i, cp))
        i += k
    return out


def bounds(b):
    """Every character boundary of b, 0 and len(b) included."""
    return [i for i, _ in chars(b)] + [len(b)]


def advance(cp):
    if cp > 0xFF:
        return FAR_W
    ch = chr(cp)
    if ch in NARROW:
        return NARROW_W
    if ch in WIDE:
        return WIDE_W
    return MID_W


def kern(prev, cp):
    if prev is None or prev > 0xFF or cp > 0xFF:
        return 0
    return KERNS.get((chr(prev), chr(cp)), 0)


def width_n(b, n):
    """The oracle: width of the first n BYTES, decoded within them and
    kerning counted INSIDE the prefix -- which is what drawing it does."""
    w, prev = 0, None
    for _, cp in chars(b, n):
        w += kern(prev, cp) + advance(cp)
        prev = cp
    return w


def index_at_x(b, x):
    """The oracle's inverse: nearest character boundary to x, as a byte."""
    if x <= 0:
        return 0
    w, prev = 0, None
    for i, cp in chars(b):
        adv = kern(prev, cp) + advance(cp)
        if x < w + adv // 2:
            return i
        w += adv
        prev = cp
    return len(b)


DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ugfx.h"

// The synthetic proportional face. These two are the only font state
// the measurement functions stand on, which is the whole reason they
// could be split out of ugfx.c into their own translation unit.
static const char *NARROW = "%(narrow)s";
static const char *WIDE   = "%(wide)s";
static int in_set(const char *set, uint32_t c) {
    for (int i = 0; set[i]; i++) if ((unsigned char)set[i] == c) return 1;
    return 0;
}
int ugfx_cp_advance(uint32_t c) {
    if (c > 0xFF) return %(far_w)d;
    if (in_set(NARROW, c)) return %(narrow_w)d;
    if (in_set(WIDE, c))   return %(wide_w)d;
    return %(mid_w)d;
}
int ugfx_char_advance(char c) { return ugfx_cp_advance((unsigned char)c); }
int ugfx_font_codepage(void) { return 0; }   // the UTF-8 mode; w16's is not tested here
int ugfx_kern(int prev, int c) {
%(kern_body)s
    (void)prev; (void)c;
    return 0;
}
int ugfx_char_w(void) { return %(cell)d; }   // the WIDEST advance
int ugfx_char_h(void) { return 16; }

// Everything uui_textbox.c draws through. None of the geometry depends
// on a pixel landing anywhere, so stubs are the whole port.
int uui_caret_visible(void) { return 1; }   // the blink phase: drawing, not geometry
void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c)
{ (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
void ugfx_draw_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c)
{ (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
int ugfx_draw_string_clipped(struct ugfx_surface *s, int x, int y, int max_w,
                              const char *str, uint32_t fg, uint32_t bg)
{ (void)s; (void)x; (void)y; (void)max_w; (void)str; (void)fg; (void)bg; return 1; }
uint32_t ugfx_rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)((r << 16) | (g << 8) | b); }
void uui_focus_ring(struct ugfx_surface *s, int x, int y, int w, int h)
{ (void)s; (void)x; (void)y; (void)w; (void)h; }
void uui_control_frame(struct ugfx_surface *s, int x, int y, int w, int h,
                       uint32_t fill, uint32_t border)
{ (void)s; (void)x; (void)y; (void)w; (void)h; (void)fill; (void)border; }
int uui_hit(int x, int y, int w, int h, int cx, int cy)
{ return cx >= x && cx < x + w && cy >= y && cy < y + h; }

// The theme, stubbed: a colour is not what this harness is about, but
// uui_textbox.c resolves them at draw time now and so must link.
struct utheme;
static struct { uint32_t c[256]; } g_stub_theme;   // >= sizeof(struct utheme)
const struct utheme *utheme_current(void) { return (const struct utheme *)&g_stub_theme; }

#include "ui/ugfx_text.c"

// INCLUDED, NOT LINKED: field_window_start() and field_avail() are
// static, and they are the two derivations the round trip is really
// about. Testing them from outside would mean inferring them from the
// hit test, which cannot see where the window starts.
#include "ui/uui_textbox.c"

uint32_t uui_state_bg(uint32_t base, enum uui_state st) { (void)st; return base; }
size_t k_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

// The clipboard verbs live in ui/uui_edit_clip.c, which needs the
// clipboard service; nothing here copies or pastes.
int uui_edit_copy(struct uui_edit *e, const struct uui_edit_ops *o, void *t)
{ (void)e; (void)o; (void)t; return 0; }
int uui_edit_cut(struct uui_edit *e, const struct uui_edit_ops *o, void *t)
{ (void)e; (void)o; (void)t; return 0; }
int uui_edit_paste(struct uui_edit *e, const struct uui_edit_ops *o, void *t)
{ (void)e; (void)o; (void)t; return 0; }

int main(void) {
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        // <op> <tab> args...
        char *nl = line; while (*nl && *nl != '\n') nl++; *nl = 0;
        if (line[0] == 0) continue;

        char *op = line;
        char *rest = line; while (*rest && *rest != '\t') rest++;
        if (*rest) *rest++ = 0;

        if (xeq(op, "width")) {
            // width <tab> n <tab> text
            int n = atoi(rest);
            char *text = rest; while (*text && *text != '\t') text++;
            if (*text) text++;
            printf("%%d\n", ugfx_text_width_n(text, n));
        } else if (xeq(op, "index")) {
            int x = atoi(rest);
            char *text = rest; while (*text && *text != '\t') text++;
            if (*text) text++;
            printf("%%d\n", ugfx_text_index_at_x(text, x));
        } else if (xeq(op, "field")) {
            // field <tab> w <tab> cursor <tab> text
            //   -> start <tab> shown_n <tab> caret_x <tab> hit(caret_x)
            int w = atoi(rest);
            char *p1 = rest; while (*p1 && *p1 != '\t') p1++; if (*p1) p1++;
            int cursor = atoi(p1);
            char *text = p1; while (*text && *text != '\t') text++;
            if (*text) text++;

            struct uui_textbox f; memset(&f, 0, sizeof f);   // a field the way an app has one: zeroed, then initialised
            uui_textbox_init(&f, text);
            uui_textbox_set_geometry(&f, 0, 0, w, 24);
            f.active = 1;
            f.ed.cursor = cursor;

            int avail = field_avail(&f);
            int start = field_window_start(&f, avail);
            int shown_n = ugfx_text_fit_chars(f.buf + start, avail);
            int caret_x = UUI_TEXTBOX_PAD + ugfx_text_width_n(f.buf + start, cursor - start);
            int back = uui_textbox_index_at_x(&f, caret_x);
            printf("%%d\t%%d\t%%d\t%%d\t%%d\n", start, shown_n, caret_x, back, avail);
        } else if (xeq(op, "step")) {
            // step <tab> i <tab> text -> next(i) <tab> prev(i)
            int i = atoi(rest);
            char *text = rest; while (*text && *text != '\t') text++;
            if (*text) text++;
            printf("%%d\t%%d\n", ugfx_text_next(text, i), ugfx_text_prev(text, i));
        } else if (xeq(op, "edit")) {
            // edit <tab> cursor <tab> key,key,... <tab> text
            //   -> the buffer's bytes in hex <tab> cursor
            int cursor = atoi(rest);
            char *keys = rest; while (*keys && *keys != '\t') keys++; if (*keys) keys++;
            char *text = keys; while (*text && *text != '\t') text++;
            if (*text) *text++ = 0;
            struct uui_textbox f; memset(&f, 0, sizeof f);
            uui_textbox_init(&f, text);
            f.active = 1;
            f.ed.cursor = cursor;
            for (char *k = keys; *k;) {
                uui_textbox_key(&f, (int)strtol(k, &k, 0));
                if (*k == ',') k++;
            }
            for (int i = 0; i < f.len; i++) printf("%%02x", (unsigned char)f.buf[i]);
            printf("\t%%d\n", f.ed.cursor);
        } else if (xeq(op, "mask")) {
            // mask <tab> text -> stars drawn <tab> star caret (cursor at the end)
            struct uui_textbox f, tmp; memset(&f, 0, sizeof f);
            uui_textbox_init(&f, rest);
            f.masked = 1;
            const struct uui_textbox *d = shown_field(&f, &tmp);
            printf("%%d\t%%d\n", d->len, d->ed.cursor);
        }
        fflush(stdout);
    }
    return 0;
}
"""

# uui_textbox.c uses these; provide them the way utext_hostcheck does.
PRELUDE = r"""
static int xeq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
"""

CASES = [
    # --- UTF-8 (docs/utf8-design.md): BYTES, decoded by the oracle ---
    "Zażółć gęślą jaźń",
    "żż",
    "AVż To",                   # kern pairs either side of a character past the atlas
    "Ελληνικά γράμματα",
    "Привет, мир",
    "╔══╗ ║ok║ ╚══╝",
    "日本語のテキスト",
    b"\xc5ngstr\xf6m p\xe5 \xe4ven",   # LATIN-1: every accent a stray byte
    b"a\xc3(b\xe2\x82 \xf0\x9f\x98",   # broken and truncated sequences
    b"\xc3\xa9\xa9\xe2\x82\xe2\x82\xac",  # a valid one, then strays around another
    "",
    "i",
    "W",
    "iiii",
    "WWWW",
    "AV",                       # a kern pair
    "To",                       # the other one
    "Wiliam",
    "illicit will",
    "AVATAR To Wit",
    "/home/user/documents/a-rather-long-file-name.txt",
    "MMMMMMMMMMMMMMMMMMMM",
    "iiiiiiiiiiiiiiiiiiii",
    "Mixed Wi.dth, i;l|t and WMmw@ together",
]

FIELD_WIDTHS = [40, 60, 120, 400]


def build(tmp, control=False, bytes_control=False):
    src = os.path.join(tmp, "ugfx_text_host.c")
    kern_body = "\n".join(
        '    if (prev == \'%s\' && c == \'%s\') return %d;' % (a, b, v)
        for (a, b), v in sorted(KERNS.items())
    )
    body = PRELUDE + DRIVER % {
        "narrow": NARROW.replace("\\", "\\\\").replace("'", "\\'"),
        "wide": WIDE,
        "narrow_w": NARROW_W,
        "wide_w": WIDE_W,
        "mid_w": MID_W,
        "far_w": FAR_W,
        "cell": CELL,
        "kern_body": kern_body,
    }
    with open(src, "w") as f:
        f.write(body)

    tb = os.path.join(ROOT, "userland", "ui", "uui_textbox.c")
    edit = os.path.join(ROOT, "userland", "ui", "uui_edit.c")
    if bytes_control:
        # THE UTF-8 CONTROL: decoding and stepping one BYTE at a time, as
        # before UTF-8 -- written into copies the driver finds first.
        os.makedirs(os.path.join(tmp, "ui"), exist_ok=True)
        os.makedirs(os.path.join(tmp, "lib"), exist_ok=True)
        u = open(os.path.join(ROOT, "userland", "lib", "uutf8.h")).read()
        u2 = u.replace("static inline uint32_t uutf8_decode(const char *s, int avail, int *len) {\n",
                       "static inline uint32_t uutf8_decode(const char *s, int avail, int *len) {\n"
                       "    if (avail == 0 || !s[0]) { if (len) *len = 0; return 0; }\n"
                       "    if (len) *len = 1; return (unsigned char)s[0];\n")
        assert u2 != u, "the bytes control matched nothing in uutf8.h"
        open(os.path.join(tmp, "lib", "uutf8.h"), "w").write(u2)
    if control:
        # THE POSITIVE CONTROL: put the cell arithmetic back, exactly as
        # it was before this was fixed, and require the suite to notice.
        text = open(tb).read()
        text = text.replace(
            "    int idx = start + ugfx_text_index_at_x(T(f) + start, rel);",
            "    int cw = ugfx_char_w();\n"
            "    int idx = start + (rel + cw / 2) / cw;")
        # **AT ui/uui_textbox.c, NOT uui_textbox.c.** The driver includes
        # it by that path, so a copy written anywhere else is simply not
        # the file that gets compiled -- the control then passes while
        # testing the unmodified code, which is what happened the first
        # time this was written.
        os.makedirs(os.path.join(tmp, "ui"), exist_ok=True)
        tb = os.path.join(tmp, "ui", "uui_textbox.c")
        with open(tb, "w") as f:
            f.write(text)
        assert text != open(os.path.join(ROOT, "userland", "ui",
                                         "uui_textbox.c")).read(), \
            "the control edit matched nothing -- the source moved under it"

    exe = os.path.join(tmp, "hostcheck")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-o", exe, src, edit,
           os.path.join(ROOT, "userland", "ui", "uui_undo.c")]   # uui_edit's undo
    if control or bytes_control:
        cmd.append("-I" + tmp)          # BEFORE the real tree: first match wins
    cmd += ["-I" + os.path.join(ROOT, "userland"),
           "-I" + os.path.join(ROOT, "userland", "include"),
           "-I" + os.path.join(ROOT, "kernel", "include", "api"),
           "-I" + os.path.join(ROOT, "kernel", "include", "abi"),
           "-I" + os.path.join(ROOT, "build", "gen"),   # kconfig.h, generated
           "-DUUI_TEXTBOX_HOSTCHECK=1"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-4000:])
        raise SystemExit("hostcheck did not compile")
    return exe


def run(exe, lines):
    """Each request is bytes or str; the cases carry raw bytes, so the
    pipe does too."""
    data = b"".join((l if isinstance(l, bytes) else l.encode()) + b"\n" for l in lines)
    r = subprocess.run([exe], input=data, capture_output=True)
    return r.stdout.decode("latin-1").splitlines()


def as_bytes(s):
    return s if isinstance(s, bytes) else s.encode("utf-8")


ZDOT = "ż".encode()          # C5 BC
LEFT, RIGHT, BKSP, DEL = 0xF784, 0xF785, 8, 0xF788   # api/keyboard.h


def edit_checks(exe, fails):
    """The editing half, read back as BYTES -- the screen cannot show a
    stray half of a character any differently from a whole one."""
    reqs, want = [], []
    def case(label, text, cursor, keys, buf, cur):
        reqs.append(b"edit\t%d\t%s\t" % (cursor, ",".join(str(k) for k in keys).encode())
                    + as_bytes(text))
        want.append((label, as_bytes(buf).hex(), cur))
    t = "a" + "ż" + "b"                                   # 61 C5BC 62
    case("one Right from 1 steps over the whole ż", t, 1, [RIGHT], t, 3)
    case("one Left from 3 steps back over the whole ż", t, 3, [LEFT], t, 1)
    case("one Backspace after ż removes both its bytes", t, 3, [BKSP], "ab", 1)
    case("one Delete before ż removes both its bytes", t, 1, [DEL], "ab", 1)
    case("a typed U+017C is inserted as C5 BC", "ab", 1, [0x17C], t, 3)
    case("a typed U+00E4 is inserted as UTF-8, not the Latin-1 byte", "", 0, [0xE4], "ä", 2)
    case("a typed CJK U+65E5 is three bytes", "", 0, [0x65E5], "日", 3)
    case("a stray Latin-1 byte is ONE character to Backspace", b"x\xe5", 2, [BKSP], "x", 1)
    case("Right over a stray byte moves one byte", b"\xe5y", 0, [RIGHT], b"\xe5y", 1)
    case("a Private Use key types nothing", "ab", 1, [0xE000], "ab", 1)
    got = run(exe, reqs)
    for (label, hx, cur), g in zip(want, got):
        parts = g.split("\t")
        if len(parts) != 2 or parts[0] != hx or int(parts[1]) != cur:
            fails.append("EDIT %s: got %r want %s, cursor %d" % (label, g, hx, cur))
    n = len(want)
    got = run(exe, [b"mask\t" + ZDOT + ZDOT, b"mask\tab\xe5"])
    for g, (label, stars) in zip(got, [("a masked żż shows two stars", 2),
                                       ("a masked ab<E5> shows three", 3)]):
        n += 1
        if g.split("\t")[0] != str(stars) or g.split("\t")[1] != str(stars):
            fails.append("MASK %s: got %r" % (label, g))
    return n


def main():
    control = "--positive-control" in sys.argv[1:]
    bytes_control = "--positive-control-bytes" in sys.argv[1:]
    fails, checks = [], 0

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, control=control, bytes_control=bytes_control)

        # --- 1. measurement against the oracle ------------------------
        reqs, want = [], []
        for s in map(as_bytes, CASES):
            for n in range(-1, len(s) + 1):
                reqs.append(b"width\t%d\t" % n + s)
                want.append(("width_n(%r, %d)" % (s, n), width_n(s, n)))
            full = width_n(s, -1)
            for x in range(-4, full + 6):
                reqs.append(b"index\t%d\t" % x + s)
                want.append(("index_at_x(%r, %d)" % (s, x), index_at_x(s, x)))
            bs = bounds(s)
            for k, i in enumerate(bs):
                reqs.append(b"step\t%d\t" % i + s)
                want.append(("next/prev(%r, %d)" % (s, i),
                             "%d\t%d" % (bs[min(k + 1, len(bs) - 1)], bs[max(k - 1, 0)])))

        got = run(exe, reqs)
        for (label, w), g in zip(want, got):
            checks += 1
            if g != str(w):
                fails.append("%s: got %s want %s" % (label, g, w))

        # --- 2. the round trip, and the window ------------------------
        reqs, meta = [], []
        for s in map(as_bytes, CASES):
            for w in FIELD_WIDTHS:
                for cursor in bounds(s):
                    reqs.append(b"field\t%d\t%d\t" % (w, cursor) + s)
                    meta.append((s, w, cursor))

        got = run(exe, reqs)
        for (s, w, cursor), row in zip(meta, got):
            start, shown_n, caret_x, back, avail = (int(v) for v in row.split("\t"))
            pad = 4

            checks += 1
            if back != cursor:
                fails.append(
                    "ROUND TRIP %r w=%d cursor=%d: the caret is drawn at x=%d, "
                    "which hit-tests back to %d" % (s, w, cursor, caret_x, back))

            # The caret must be INSIDE the field: that is what the
            # window exists for.
            checks += 1
            if not (pad <= caret_x <= pad + avail):
                fails.append(
                    "CARET OUTSIDE %r w=%d cursor=%d: caret_x=%d, field inner is %d..%d"
                    % (s, w, cursor, caret_x, pad, pad + avail))

            # Never scrolled further than necessary: at start > 0 the
            # caret must not fit with one character less of scroll.
            if start > 0:
                checks += 1
                before = max(b for b in bounds(s) if b < start)
                if start not in bounds(s) or width_n(s[before:cursor], -1) <= avail:
                    fails.append(
                        "OVER-SCROLLED %r w=%d cursor=%d: start=%d but start-1 fits too"
                        % (s, w, cursor, start))

            # Nothing drawn past the inner edge.
            checks += 1
            if width_n(s[start:start + shown_n], -1) > avail:
                fails.append(
                    "OVERFLOW %r w=%d: %d shown chars measure %d > %d"
                    % (s, w, shown_n, width_n(s[start:start + shown_n], -1), avail))

        checks += edit_checks(exe, fails)

    if control:
        if fails:
            print("positive control ok: %d/%d checks went RED with the cell "
                  "arithmetic restored" % (len(fails), checks))
            print("  e.g. %s" % fails[0])
            return 0
        print("positive control FAILED: the suite passed with the BUG put back.")
        print("  %d checks ran and none of them could see it." % checks)
        return 1
    if bytes_control:
        utf = [f for f in fails if any(m in f for m in ("EDIT", "MASK", "next/prev", "\\x"))]
        for kind in ("EDIT", "MASK"):
            print("  %s checks red: %d" % (kind, sum(1 for f in fails if f.startswith(kind))))
        if utf:
            print("positive control ok: %d/%d checks went RED with byte-at-a-time "
                  "text restored" % (len(fails), checks))
            print("  e.g. %s" % utf[0])
            return 0
        print("positive control FAILED: the UTF-8 checks passed decoding BYTES.")
        return 1

    if fails:
        print("%d/%d checks FAILED:\n" % (len(fails), checks))
        for f in fails[:25]:
            print("  " + f)
        if len(fails) > 25:
            print("  ... and %d more" % (len(fails) - 25))
        return 1

    print("ugfx text measurement + uui_textbox geometry ok (%d checks, "
          "%d cases x %d field widths)" % (checks, len(CASES), len(FIELD_WIDTHS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
