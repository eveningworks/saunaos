#!/usr/bin/env python3
"""Check userland/lib/uhlp.c (the Windows Help file reader) on the HOST.

WHAT THIS ADDS. The reader is exercised against Microsoft's own 3.11 help files on a
machine that has them (they are never in the repository), which proves it reads REAL
files and nothing else. This writes help files from `tools/hlpgen.py`, so the answer
is KNOWN, and it compiles the same .c file with the host gcc, under ASan and UBSan:

  - the container: the directory tree (several levels, so the index pages are walked),
    a compressed multi-block |TOPIC, the phrase table, |FONT, |SYSTEM's records
  - the lookups: every context ID's hash finds its own topic, an unknown one finds
    none, a map number, the browse sequence, the keywords and their topics
  - what a topic holds: its text (compared with the model, not a snapshot), its
    links by kind, its macros, its table
  - uhlp_hash against an independent Python implementation, and uhlp_id_for_hash:
    the string it returns hashes back to the number it was given (that is how a
    topic reached by number or keyword is named to WINHELP.EXE)
  - HOSTILE INPUT: the same files with bytes flipped, ranges zeroed, sizes set to
    0xFFFFFFFF and the end cut off must open or refuse -- never read outside the
    file, overflow, or leak (ASan, UBSan, leak check)

  python3 tools/uhlp_hostcheck.py
  python3 tools/uhlp_hostcheck.py --fuzz 4000 --seed 7
  python3 tools/uhlp_hostcheck.py --positive-control

**A CLEAN RUN PROVES NOTHING UNTIL --positive-control HAS GONE RED.** It runs two
controls on a patched COPY of the reader: a wrong hash multiplier must fail the
lookups, and a bounds check that always says yes must be caught by the fuzz.
Exit status is non-zero on any violation. Needs gcc.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import hlpgen
import hostcheck

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
       "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer")

DRIVER = r"""
#include "lib/uhlp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void esc(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') fputs("\\n", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else if (c == '|') fputs("\\p", stdout);
        else if (c < 0x20 || c > 0x7e) printf("\\x%02x", c);
        else putchar(c);
    }
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc((size_t)len + 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(p); return 0; }
    fclose(f);
    *n = (size_t)len;
    return p;
}

// Everything a caller can ask of an opened file; the fuzz runs this on whatever opened.
static unsigned long exercise(struct uhlp *h) {
    unsigned long sum = 0;
    sum += (unsigned long)uhlp_ntopics(h) + (unsigned long)uhlp_nkeywords(h) + (unsigned long)uhlp_contents(h);
    sum += strlen(uhlp_title(h));
    for (int i = 0; i < uhlp_nfonts(h); i++) sum += (unsigned long)uhlp_font(h, i)->half_pts;
    for (int i = 0; i < uhlp_nwindows(h); i++) sum += (unsigned long)uhlp_window(h, i)->w;
    for (int i = 0; i < uhlp_nmacros(h); i++) sum += strlen(uhlp_macro(h, i));
    int nt = uhlp_ntopics(h);
    for (int t = 0; t < nt && t < 400; t++) {
        sum += strlen(uhlp_topic_title(h, t));
        sum += (unsigned long)(uhlp_topic_prev(h, t) + uhlp_topic_next(h, t));
        sum += uhlp_topic_hash(h, t);
        struct uhlp_doc *d = uhlp_render(h, t);
        if (d) {
            for (int p = 0; p < d->npara; p++) {
                const struct uhlp_para *pr = &d->para[p];
                if (pr->run0 < 0 || pr->nrun < 0 || pr->run0 + pr->nrun > d->nrun) abort();
                for (int r = 0; r < pr->nrun; r++) {
                    const struct uhlp_run *ru = &d->run[pr->run0 + r];
                    if (ru->kind == UHLP_RUN_TEXT) sum += strlen(uhlp_doc_str(d, ru->text));
                    if (ru->link >= d->nlink) abort();
                    if (ru->kind == UHLP_RUN_PICTURE && (ru->pic < 0 || ru->pic >= d->npic)) abort();
                    if (ru->font >= uhlp_nfonts(h) && uhlp_nfonts(h) > 0) abort();
                }
                if (pr->table >= d->ntable) abort();
            }
            sum += (unsigned long)d->nhot;
            uhlp_doc_free(d);
        }
        char *tx = uhlp_topic_text(h, t);
        if (tx) { sum += strlen(tx); free(tx); }
    }
    for (int k = 0; k < uhlp_nkeywords(h) && k < 400; k++) {
        int n;
        const int *tp = uhlp_keyword_topics(h, k, &n);
        for (int i = 0; i < n; i++) if (tp[i] < 0 || tp[i] >= nt) abort();
        sum += strlen(uhlp_keyword(h, k));
    }
    sum += (unsigned long)(uhlp_topic_by_hash(h, uhlp_hash("details")) + uhlp_topic_by_map(h, 100));
    return sum;
}

static void dump(struct uhlp *h) {
    printf("H|%d|%s|%s|%d|%d|%d|%d|%d|%d\n", uhlp_version(h), uhlp_title(h),
           uhlp_copyright(h) ? uhlp_copyright(h) : "-", uhlp_ntopics(h), uhlp_contents(h),
           uhlp_nfonts(h), uhlp_nwindows(h), uhlp_nmacros(h), uhlp_nkeywords(h));
    for (int i = 0; i < uhlp_nfonts(h); i++) {
        const struct uhlp_font *f = uhlp_font(h, i);
        printf("F|%d|%s|%d|%d|%d|%06x\n", i, f->face, f->half_pts, f->flags, f->family, f->rgb);
    }
    for (int i = 0; i < uhlp_nwindows(h); i++) {
        const struct uhlp_window *w = uhlp_window(h, i);
        printf("W|%d|%s|%s|%d|%d|%d|%d|%06x|%06x\n", i, w->name, w->caption, w->x, w->y, w->w, w->h, w->sr_rgb, w->nsr_rgb);
    }
    for (int i = 0; i < uhlp_nmacros(h); i++) printf("M|%d|%s\n", i, uhlp_macro(h, i));
    for (int t = 0; t < uhlp_ntopics(h); t++) {
        printf("T|%d|%s|%d|%d|%08x\n", t, uhlp_topic_title(h, t), uhlp_topic_prev(h, t), uhlp_topic_next(h, t), uhlp_topic_hash(h, t));
        struct uhlp_doc *d = uhlp_render(h, t);
        if (!d) { printf("D|%d|FAILED\n", t); continue; }
        printf("D|%d|%d|%d|%d|%d|%d|%d\n", t, d->npara, d->nrun, d->nlink, d->ntable, d->npic, d->nmacro);
        for (int m = 0; m < d->nmacro; m++) printf("TM|%d|%d|%s\n", t, m, d->macro[m]);
        for (int l = 0; l < d->nlink; l++) {
            const struct uhlp_link *k = &d->link[l];
            printf("L|%d|%d|%d|%d|%08x|%d|%d|%s|%s\n", t, l, k->kind, k->colour, k->hash, k->by_offset, k->window,
                   k->file >= 0 ? uhlp_doc_str(d, k->file) : "-", k->macro >= 0 && k->kind == UHLP_LINK_MACRO ? uhlp_doc_str(d, k->macro) : "-");
        }
        for (int p = 0; p < d->npara; p++) {
            const struct uhlp_para *pr = &d->para[p];
            printf("P|%d|%d|%d|%d|%d|%d|%d\n", t, p, pr->nrun, pr->table, pr->column, pr->row, pr->scrolls);
            for (int r = 0; r < pr->nrun; r++) {
                const struct uhlp_run *ru = &d->run[pr->run0 + r];
                printf("R|%d|%d|%d|%d|%d|", t, p, ru->kind, ru->font, ru->link);
                if (ru->kind == UHLP_RUN_TEXT) esc(uhlp_doc_str(d, ru->text), (size_t)ru->len);
                putchar('\n');
            }
        }
        for (int i = 0; i < d->ntable; i++)
            printf("TB|%d|%d|%d|%d|%d|%d\n", t, i, d->table[i].ncol, d->table[i].width, d->table[i].type, d->table[i].nrow);
        uhlp_doc_free(d);
        char *tx = uhlp_topic_text(h, t);
        printf("X|%d|", t);
        if (tx) { esc(tx, strlen(tx)); free(tx); }
        putchar('\n');
    }
    for (int k = 0; k < uhlp_nkeywords(h); k++) {
        int n;
        const int *tp = uhlp_keyword_topics(h, k, &n);
        printf("K|%d|%s", k, uhlp_keyword(h, k));
        for (int i = 0; i < n; i++) printf("|%d", tp[i]);
        putchar('\n');
    }
}

static unsigned long long rng_state;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 16);
}

static void mutate(unsigned char *b, size_t *n) {
    int kind = (int)(rnd() % 6);
    size_t len = *n;
    if (len < 40) return;
    if (kind == 0) { for (int i = 0, c = 1 + (int)(rnd() % 8); i < c; i++) b[rnd() % len] ^= (unsigned char)(1u << (rnd() % 8)); }
    else if (kind == 1) { for (int i = 0, c = 1 + (int)(rnd() % 4); i < c; i++) b[rnd() % len] = (unsigned char)rnd(); }
    else if (kind == 2) { size_t a = rnd() % len, z = a + rnd() % 64; if (z > len) z = len; memset(b + a, 0, z - a); }
    else if (kind == 3) { size_t a = rnd() % (len - 4); memset(b + a, 0xFF, 4); }
    else if (kind == 4) { *n = 16 + rnd() % (len - 16); }
    else { size_t a = rnd() % (len - 4); b[a] = (unsigned char)rnd(); b[a + 1] = (unsigned char)rnd(); b[a + 3] = 0x7F; }
}

// dump <file> | fuzz <file> <seed> <iterations> | idfor <hash>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (strcmp(argv[1], "idfor") == 0) {
        char out[16];
        uint32_t want = (uint32_t)strtoul(argv[2], 0, 16);
        if (!uhlp_id_for_hash(want, out, sizeof out)) { puts("NONE"); return 0; }
        printf("%s %08x\n", out, uhlp_hash(out));
        return 0;
    }
    size_t n;
    unsigned char *file = slurp(argv[2], &n);
    if (!file) return 2;
    if (strcmp(argv[1], "dump") == 0) {
        const char *err = 0;
        struct uhlp *h = uhlp_open(file, n, &err);
        if (!h) { printf("OPENFAIL|%s\n", err ? err : ""); return 1; }
        dump(h);
        exercise(h);
        uhlp_close(h);
        free(file);
        return 0;
    }
    if (strcmp(argv[1], "fuzz") != 0 || argc < 5) return 2;
    rng_state = 0x9E3779B97F4A7C15ull ^ strtoull(argv[3], 0, 10);
    int iters = atoi(argv[4]), opened = 0;
    unsigned char *copy = malloc(n);
    for (int it = 0; it < iters; it++) {
        memcpy(copy, file, n);
        size_t len = n;
        mutate(copy, &len);
        if (rnd() % 3 == 0) mutate(copy, &len);
        const char *err = 0;
        struct uhlp *h = uhlp_open(copy, len, &err);
        if (!h) continue;
        opened++;
        (void)exercise(h);
        uhlp_close(h);
    }
    printf("fuzz: %d runs, %d still opened\n", iters, opened);
    free(copy);
    free(file);
    return 0;
}
"""


def build(tmp, patch=None):
    drv = hostcheck.write(tmp, "uhlp_host.c", DRIVER)
    src = hostcheck.stage(tmp, "userland/lib/uhlp.c", dest="lib/uhlp.c", edits=patch or (),
                          apply=bool(patch), tool="uhlp_hostcheck")
    return hostcheck.compile(tmp, "uhlp_host", [drv, src], flags=SAN,
                             includes=[os.path.join(ROOT, "userland")], tool="uhlp_hostcheck")


def run(exe, *args, check=True):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
    r = subprocess.run([exe, *[str(a) for a in args]], capture_output=True, env=env, timeout=600)
    if check and r.returncode != 0:
        raise RuntimeError("driver failed (%d): %s%s" % (r.returncode, r.stdout.decode(errors="replace")[-400:],
                                                         r.stderr.decode(errors="replace")[-1800:]))
    return r.stdout.decode("latin-1")


def unesc(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            if c == "n":
                out.append("\n"); i += 2
            elif c == "p":
                out.append("|"); i += 2
            elif c == "\\":
                out.append("\\"); i += 2
            elif c == "x":
                out.append(chr(int(s[i + 2:i + 4], 16))); i += 4
            else:
                out.append(c); i += 2
        else:
            out.append(s[i]); i += 1
    return "".join(out)


def model_text(h, t):
    """What uhlp_topic_text must return for topic t of generator model h."""
    def para(items):
        s = ""
        for it in items:
            if it[0] == "text":
                s += it[1]
            elif it[0] in ("br", "tab"):
                s += " "
            elif it[0] in ("link", "macro"):
                s += it[1]
        return s + "\n"
    out = ""
    for row in (t.table or []):
        for cell in row:
            out += para(cell)
    for p in t.paras:
        out += para(p)
    return out


def check_model(exe, tmp, problems):
    h = hlpgen.sample()
    path = os.path.join(tmp, "sample.hlp")
    data = h.build()
    open(path, "wb").write(data)
    lines = run(exe, "dump", path).splitlines()
    rec = {}
    for ln in lines:
        parts = ln.split("|")
        rec.setdefault(parts[0], []).append(parts[1:])

    def need(cond, msg):
        if not cond:
            problems.append(msg)

    hd = rec["H"][0]
    need(hd[0] == "21", "version %s, wanted 21" % hd[0])
    need(hd[1] == h.title, "title %r" % hd[1])
    need(hd[2] == h.copyright, "copyright %r" % hd[2])
    need(int(hd[3]) == len(h.topics), "ntopics %s, wanted %d" % (hd[3], len(h.topics)))
    need(int(hd[4]) == h.contents, "contents %s" % hd[4])
    need(int(hd[5]) == len(h.fonts), "nfonts %s" % hd[5])
    need(int(hd[6]) == 1 and int(hd[7]) == len(h.macros), "windows/macros %s/%s" % (hd[6], hd[7]))

    for i, (face, half, attr, fam, rgb) in enumerate(h.fonts):
        f = rec["F"][i]
        need(f[1] == face and int(f[2]) == half and int(f[3]) == attr and int(f[4]) == fam and int(f[5], 16) == rgb,
             "font %d read as %r" % (i, f))
    w = rec["W"][0]
    need(w[1] == "side" and w[2] == "Side" and [int(x) for x in w[3:7]] == [10, 20, 300, 400], "window read as %r" % w)
    need(int(w[7], 16) == 0xFFFFFF and int(w[8], 16) == 0xE0E0E0, "window colours %r" % w[7:9])
    need([m[1] for m in rec["M"]] == h.macros, "file macros %r" % rec["M"])

    n = len(h.topics)
    tops = {int(t[0]): t for t in rec["T"]}
    for ti, t in enumerate(h.topics):
        row = tops.get(ti)
        if row is None:
            problems.append("topic %d missing" % ti)
            continue
        need(row[1] == t.title, "topic %d title %r, wanted %r" % (ti, row[1], t.title))
        need(int(row[2]) == (ti - 1 if ti else -1), "topic %d prev %s" % (ti, row[2]))
        need(int(row[3]) == (ti + 1 if ti + 1 < n else -1), "topic %d next %s" % (ti, row[3]))
        if t.ids:
            need(int(row[4], 16) in {hlpgen.hlp_hash(i) for i in t.ids}, "topic %d hash %s is none of its ids" % (ti, row[4]))
    texts = {int(x[0]): unesc("|".join(x[1:])) for x in rec["X"]}
    for ti, t in enumerate(h.topics):
        need(texts.get(ti) == model_text(h, t), "topic %d text %r, wanted %r" % (ti, texts.get(ti), model_text(h, t)))
    tm = [(int(x[0]), x[2]) for x in rec.get("TM", [])]
    need(tm == [(1, 'SaveMark("seen")')], "topic macros %r" % tm)

    # links of Contents: a jump and a popup, by hash, then Details': a macro and a hidden jump
    lk = {}
    for x in rec["L"]:
        lk.setdefault(int(x[0]), []).append(x[2:])
    c = lk.get(0, [])
    need(len(c) == 2, "Contents has %d links, wanted 2" % len(c))
    if len(c) == 2:
        need(c[0][0] == "0" and int(c[0][2], 16) == hlpgen.hlp_hash("details"), "jump link read as %r" % c[0])
        need(c[1][0] == "1" and int(c[1][2], 16) == hlpgen.hlp_hash("gloss"), "popup link read as %r" % c[1])
    d = lk.get(1, [])
    need(len(d) == 2, "Details has %d links, wanted 2" % len(d))
    if len(d) == 2:
        need(d[0][0] == "2" and d[0][6] == "Test(1)", "macro link read as %r" % d[0])
        need(d[1][0] == "0" and d[1][1] == "0" and int(d[1][2], 16) == hlpgen.hlp_hash("contents"),
             "hidden jump read as %r" % d[1])

    # the table: ONE table of two columns and two rows, cells in reading order
    tb = [x for x in rec["TB"] if x[0] == "1"]
    need(len(tb) == 1, "Details' two-row table became %d tables" % len(tb))
    if tb:
        need(int(tb[0][2]) == 2 and int(tb[0][5]) == 2, "table read as %r" % tb[0])
    paras = [x for x in rec["P"] if x[0] == "1"]
    cells = [(int(p[4]), int(p[5])) for p in paras if int(p[3]) >= 0]
    need(cells == [(0, 0), (1, 0), (0, 1), (1, 1)], "table cells (column,row) %r" % cells)

    # the keywords, sorted, with their topics
    kws = {x[1]: [int(t) for t in x[2:]] for x in rec["K"]}
    need(kws.get("Overview") == [0, 1], "Overview -> %r" % kws.get("Overview"))
    need(kws.get("Glossary") == [2], "Glossary -> %r" % kws.get("Glossary"))
    need(kws.get("Filler") == [3, 9, 15, 21], "Filler -> %r" % kws.get("Filler"))
    names = [x[1] for x in rec["K"]]
    need(names == sorted(names, key=str.lower), "keywords not in order: %r" % names)


def check_lookups(exe, tmp, problems):
    """uhlp_hash against the Python hash, and a hash found for a string and back."""
    for s in ("details", "contents", "idh_contents", "Filler7", "a.b_c9"):
        out = run(exe, "idfor", format(hlpgen.hlp_hash(s), "x"))
        parts = out.split()
        if parts[0] == "NONE" or int(parts[1], 16) != hlpgen.hlp_hash(s) or hlpgen.hlp_hash(parts[0]) != hlpgen.hlp_hash(s):
            problems.append("uhlp_id_for_hash(%08x) gave %r (python hashes it to %08x)"
                            % (hlpgen.hlp_hash(s), out.strip(), hlpgen.hlp_hash(parts[0]) if parts[0] != "NONE" else 0))
    # the file's own lookups are checked by the topics' hashes in check_model: a topic's reported
    # hash is one of ITS ids' hashes, which only holds if |CONTEXT was walked and matched.


def check_fuzz(exe, path, seed, iters, problems):
    r = subprocess.run([exe, "fuzz", path, str(seed), str(iters)], capture_output=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1"),
                       timeout=1800)
    if r.returncode != 0:
        problems.append("fuzz seed %d: exit %d\n%s" % (seed, r.returncode, r.stderr.decode(errors="replace")[:1500]))
        return None
    return r.stdout.decode().strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fuzz", type=int, default=1500, help="mutated files to open per seed")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()

    if args.positive_control:
        return positive_control(args)

    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        check_model(exe, tmp, problems)
        check_lookups(exe, tmp, problems)
        # a second file: one block per few records, so records land in many blocks, and no phrases
        small = hlpgen.sample()
        small.block_limit = 400
        small.phrases = []
        p2 = os.path.join(tmp, "small.hlp")
        open(p2, "wb").write(small.build())
        out = run(exe, "dump", p2)
        if out.count("\nT|") + out.startswith("T|") != len(small.topics):
            problems.append("many-block file: wrong topic count")
        for ln in out.splitlines():
            if ln.startswith("D|") and ln.endswith("FAILED"):
                problems.append("many-block file: %s" % ln)
        said = None
        for seed in (args.seed, args.seed + 1):
            said = check_fuzz(exe, os.path.join(tmp, "sample.hlp"), seed, args.fuzz, problems)
        check_fuzz(exe, p2, args.seed + 2, args.fuzz // 2, problems)
    if problems:
        for p in problems:
            print("FAIL:", p)
        print("uhlp_hostcheck: %d problems" % len(problems))
        return 1
    print("uhlp_hostcheck: ok (%s)" % (said or "no fuzz"))
    return 0


def positive_control(args):
    """Two patched copies of the reader: each must be CAUGHT, by the half of the check built for it."""
    rc = 0
    controls = [
        ("a wrong hash multiplier", [("hash = hash * 43 + x;", "hash = hash * 41 + x;")], "model"),
        ("a bounds check that always agrees",
         [("static int v_ok(struct view v, size_t off, size_t len) { return off <= v.n && len <= v.n - off; }",
           "static int v_ok(struct view v, size_t off, size_t len) { (void)v; (void)off; (void)len; return 1; }")],
         "fuzz"),
    ]
    for name, patch, half in controls:
        problems = []
        with tempfile.TemporaryDirectory() as tmp:
            exe = build(tmp, patch)
            try:
                if half == "model":
                    check_model(exe, tmp, problems)
                    check_lookups(exe, tmp, problems)
                else:
                    path = os.path.join(tmp, "sample.hlp")
                    open(path, "wb").write(hlpgen.sample().build())
                    check_fuzz(exe, path, args.seed, args.fuzz, problems)
            except RuntimeError as e:
                problems.append(str(e)[:200])
        if problems:
            print("control %-36s RED (good): %s" % (name, problems[0].splitlines()[0][:100]))
        else:
            print("control %-36s STAYED GREEN -- this check cannot see that break" % name)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
