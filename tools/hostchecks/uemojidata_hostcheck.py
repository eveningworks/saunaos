#!/usr/bin/env python3
"""The emoji list's parser, search and recents (base/uemojidata.c), on the HOST.

`uemojidata.c` reads /usr/share/unicode/emoji -- the shipped
data/usr/share/unicode/emoji -- into groups and entries, searches names, and
keeps the recently used row as one settings value. This compiles the tree's
uemojidata.c with the host gcc (the file and settings calls stubbed) and
compares it with this script's own reading of the same file, which shares no
code with it:

  - every entry: its characters (a sequence whole, U+FE0F where the file
    says), its group and its name, and a people emoji's five SKIN TONES made
    from its template; every group's name, tab word, first entry and count;
  - searches by substring of the name with the case changed ("HEART", "Cat");
  - the recents value, a whole emoji at a time: a pick moved to the front, a
    duplicate dropped, the row cut at 16 and at the settings ABI's 63 bytes,
    and a value decoded back to entries (a family whole, an unknown character
    skipped); a pasted emoji, toned or not, found as its entry.

`--positive-control` breaks three things in a copy -- a case-sensitive
search, recents that keep duplicates, a tone template's slot left empty --
and each must go RED.

  python3 tools/hostchecks/uemojidata_hostcheck.py [--positive-control]
"""

RUNNER = {"name": "uemojidata_host", "covers": "the emoji list's parse, name search and recents value",
          "gate": True, "parallel": True}

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = toolpath.REPO
TOOL = "uemojidata_hostcheck"
DATA = os.path.join(ROOT, "data", "usr", "share", "unicode", "emoji")
QUERIES = ["\u00f1", "\u2019", "heart", "HEART", "Cat", "face with", "o’clock", "zzzqqq", "x", "\U0001F600", "\u2764\ufe0f",
           "\u2764", "\U0001F600\U0001F600", "\U0001F1EB\U0001F1EE", "\U0001F44B\U0001F3FD", "flag: f"]
VALUE_MAX = 64          # SETTING_ABI_VALUE_MAX
RECENT_MAX = 16

STUBS = {
    "base/ufile.h": r"""
#include <stddef.h>
#include <stdint.h>
enum ufile_result { UFILE_OK, UFILE_ERR };
enum ufile_result ufile_slurp(const char *path, size_t cap, uint8_t **out, size_t *out_len);
""",
    "base/usetting.h": r"""
#include <stddef.h>
#define SETTING_ABI_VALUE_MAX 64
int usetting_get(const char *name, char *out, size_t cap);
int usetting_set(const char *name, const char *value);
""",
}

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/uemojidata.h"

enum ufile_result { UFILE_OK, UFILE_ERR };
static char g_set[64];
int usetting_get(const char *name, char *out, size_t cap) { (void)name; snprintf(out, cap, "%s", g_set); return 1; }
int usetting_set(const char *name, const char *value) { (void)name; snprintf(g_set, sizeof g_set, "%s", value); return 1; }
int ufile_slurp(const char *path, size_t cap, unsigned char **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return UFILE_ERR;
    *out = malloc(cap); *n = fread(*out, 1, cap, f); fclose(f); return UFILE_OK;
}

static void hexs(const char *s) { for (; *s; s++) printf("%02X", (unsigned char)*s); }

int main(int argc, char **argv) {
    (void)argc;
    FILE *f = fopen(argv[1], "rb");
    char *text = malloc(1 << 20);
    int len = (int)fread(text, 1, (1 << 20) - 1, f);
    text[len] = 0;
    fclose(f);
    struct uemojidata d;
    if (!uemojidata_parse(text, len, &d)) return 1;
    for (int g = 0; g < d.ngroups; g++)
        printf("G\t%s\t%s\t%d\t%d\n", d.groups[g].name, d.groups[g].tab, d.groups[g].first, d.groups[g].count);
    for (int i = 0; i < d.count; i++) {
        printf("E\t%X\t%d\t%d\t%s\t", d.entries[i].cp, d.entries[i].vs16, d.entries[i].group, d.entries[i].name);
        hexs(d.entries[i].seq);
        printf("\n");
        for (int t = 1; t <= UEMOJI_TONES && d.entries[i].toned; t++) {
            char out[128];
            int n = uemojidata_toned(&d.entries[i], t, out, sizeof out);
            printf("T\t%d\t%d\t%d\t", i, t, n);
            hexs(n ? out : "");
            printf("\n");
        }
    }
    // Queries, one per line on stdin, then recents cases.
    char line[1024];
    static int out[4096];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "Q\t", 2)) {
            int n = uemojidata_search(&d, line + 2, out, 4096);
            printf("Q\t%d", n);
            for (int i = 0; i < n && i < 4096; i++) printf(" %d", out[i]);
            printf("\n");
        } else if (!strncmp(line, "P\t", 2)) {
            // P <hex value> <cp>: the push of cp onto the value
            char *sp = strchr(line + 2, '\t');
            *sp = 0;
            char was[256] = "", now[64];
            const char *h = line + 2;
            int k = 0;
            for (; h[0] && h[1]; h += 2) { unsigned b; sscanf(h, "%2x", &b); was[k++] = (char)b; }
            was[k] = 0;
            char seq[128] = "";
            int m = 0;
            for (const char *x = sp + 1; x[0] && x[1]; x += 2) { unsigned b; sscanf(x, "%2x", &b); seq[m++] = (char)b; }
            seq[m] = 0;
            int n = uemojidata_recent_push(was, seq, now, sizeof now);
            printf("P\t%d\t", n); hexs(now); printf("\n");
        } else if (!strncmp(line, "D\t", 2)) {
            char was[256] = "";
            const char *h = line + 2;
            int k = 0;
            for (; h[0] && h[1]; h += 2) { unsigned b; sscanf(h, "%2x", &b); was[k++] = (char)b; }
            was[k] = 0;
            int n = uemojidata_recent_decode(&d, was, out, 64);
            printf("D\t%d", n);
            for (int i = 0; i < n; i++) printf(" %d", out[i]);
            printf("\n");
        }
    }
    return 0;
}
"""

CONTROLS = [
    ("a case-sensitive search",
     "static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }",
     "static char lower(char c) { return c; }"),
    ("recents keep duplicates",
     "if ((sn && same_emoji(value + i, k, seq)) || (k == 3 && !memcmp(value + i, \"\\xEF\\xB8\", 2))) continue;",
     "if (k == 3 && !memcmp(value + i, \"\\xEF\\xB8\", 2)) continue;"),
    ("a tone template's slot left empty",
     "        if (*p == UEMOJI_TONE_SLOT) { memcpy(out + o, u, (size_t)t); o += t; }",
     "        if (*p == UEMOJI_TONE_SLOT) { }"),
]


def oracle():
    """This script's own reading of the file: groups, entries, tone forms."""
    groups, entries, toned = [], [], {}
    for raw in open(DATA, encoding="utf-8"):
        raw = raw.rstrip("\n")
        if raw.startswith("@"):
            name = raw[1:]
            groups.append([name, name.split(" ")[0][:15], len(entries), 0])
        elif raw and not raw.startswith("#") and groups:
            fields = raw.split(";")
            cps, name = [int(x, 16) for x in fields[0].split()], fields[1]
            if len(fields) > 2:
                tpl = fields[2].split()
                toned[len(entries)] = ["".join(chr(0x1F3FA + t) if x == "~" else chr(int(x, 16)) for x in tpl)
                                       for t in range(1, 6)]
            entries.append((cps[0], int(len(cps) == 2 and cps[1] == 0xFE0F), len(groups) - 1, name,
                            "".join(chr(c) for c in cps).encode().hex().upper(), "".join(map(chr, cps))))
            groups[-1][3] += 1
    return groups, entries, toned


def push(units, seq):
    """The recents rule, written again over whole emoji: front, no
    duplicates, 16 and 63 bytes."""
    out = [seq]
    for u in units:
        if u.replace("️", "") == seq.replace("️", "") or len(out) >= RECENT_MAX:
            continue
        if len("".join(out + [u]).encode()) > VALUE_MAX - 1:
            break
        out.append(u)
    return "".join(out)


def cases(entries):
    by = {e[3]: e[5] for e in entries}
    smile, heart, party, cat = by["grinning face"], by["red heart"], by["party popper"], by["cat face"]
    fam, fin = by["family: man, woman, girl"], by["flag: Finland"]
    many = [e[5] for e in entries if e[0] > 0xFFFF and len(e[5]) == 1][:20]
    return [
        ([], smile),
        ([party, smile], smile),                  # a repeat moves to the front, once
        ([heart, cat], party),
        (many, heart),                            # cut at 16 and at 63 bytes
        (many[:15], smile),
        ([fam, fin, smile], fin),                 # sequences are whole emoji
        ([fin, fam], cat),
    ]


def run(exe, quiet=False):
    groups, entries, toned = oracle()
    pushes = cases(entries)
    stdin = "".join(f"Q\t{q}\n" for q in QUERIES)
    stdin += "".join(f"P\t{''.join(v).encode().hex()}\t{seq.encode().hex()}\n" for v, seq in pushes)
    fam = [e[5] for e in entries if e[3] == "family: man, woman, girl"][0]
    decode_units = [chr(0x1F431), "z", chr(0x2764), fam, chr(0x1F600)]
    decode_in = "".join(decode_units)
    stdin += f"D\t{decode_in.encode().hex()}\n"
    r = subprocess.run([exe, DATA], input=stdin, capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        sys.exit(f"{TOOL}: driver failed: {r.stderr}")
    lines = r.stdout.splitlines()
    bad = 0

    def check(ok, what):
        nonlocal bad
        bad += not ok
        if not quiet:
            print(f"  {'ok  ' if ok else 'FAIL'} {what}")

    got_g = [ln.split("\t")[1:] for ln in lines if ln.startswith("G\t")]
    want_g = [[g[0], g[1], str(g[2]), str(g[3])] for g in groups]
    check(got_g == want_g, f"{len(want_g)} groups: names, tab words, first entries and counts")
    got_e = [ln.split("\t")[1:] for ln in lines if ln.startswith("E\t")]
    want_e = [["%X" % e[0], str(e[1]), str(e[2]), e[3], e[4]] for e in entries]
    check(got_e == want_e, f"{len(want_e)} entries: characters, U+FE0F, group, name "
                           f"({sum(e[1] for e in entries)} with the selector, "
                           f"{sum(len(e[5]) > 1 + e[1] for e in entries)} sequences)")
    got_t = [ln.split("\t")[1:] for ln in lines if ln.startswith("T\t")]
    want_t = [[str(i), str(t), str(len(f.encode())), f.encode().hex().upper()]
              for i, forms in sorted(toned.items()) for t, f in enumerate(forms, 1)]
    check(got_t == want_t and len(toned) >= 200, f"{len(toned)} entries' five skin tones, from the template")
    got_q = [ln for ln in lines if ln.startswith("Q\t")]
    for q, line in zip(QUERIES, got_q):
        bare = q.replace("\ufe0f", "")
        one = [i for i, e in enumerate(entries)
               if bare == e[5].replace("\ufe0f", "") or bare in [f.replace("\ufe0f", "") for f in toned.get(i, [])]]
        want = one[:1] or [i for i, e in enumerate(entries) if q.lower() in e[3].lower()]
        parts = line.split("\t")[1].split()
        check(int(parts[0]) == len(want) and list(map(int, parts[1:])) == want,
              f"search {q!r}: {len(want)} match(es)")
    got_p = [ln.split("\t") for ln in lines if ln.startswith("P\t")]
    for (v, seq), line in zip(pushes, got_p):
        want = push(v, seq)
        got = bytes.fromhex(line[2]).decode("utf-8", errors="replace") if len(line) > 2 else ""
        check(got == want and int(line[1]) == len(want.encode()),
              f"recents: {seq!r} onto {len(v)} emoji -> {len(want.encode())} bytes")
    d = [ln for ln in lines if ln.startswith("D\t")]
    want_d = []
    for u in decode_units:
        hit = [i for i, e in enumerate(entries) if e[5].replace("\ufe0f", "") == u.replace("\ufe0f", "")]
        if hit:
            want_d.append(hit[0])
    check(bool(d) and list(map(int, d[0].split("\t")[1].split()[1:])) == want_d,
          "a recents value decodes to its entries, a sequence whole, an unknown character skipped")
    return bad


def build(tmp, control=None):
    inc = os.path.join(tmp, "inc")
    for rel, body in STUBS.items():
        hostcheck.write(inc, rel, body)
    hostcheck.stage(inc, "userland/libs/base/uemojidata.h", dest="base/uemojidata.h", tool=TOOL)
    hostcheck.stage(inc, "userland/libs/base/uutf8.h", dest="base/uutf8.h", tool=TOOL)
    hostcheck.stage(inc, "kernel/include/api/kutf8.h", tool=TOOL)
    hostcheck.stage(inc, "kernel/include/api/kgrapheme.h", tool=TOOL)
    hostcheck.stage(inc, "kernel/include/api/kgrapheme_table.h", tool=TOOL)
    seg = hostcheck.stage(tmp, "kernel/lib/kgrapheme.c", tool=TOOL)
    edits = [(control[1], control[2])] if control else []
    src = hostcheck.stage(tmp, "userland/libs/base/uemojidata.c", edits=edits, apply=bool(edits), tool=TOOL)
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "uemojidata_check", [drv, src, seg], flags=["-O2", "-Wall", "-Wextra"],
                             includes=[inc], tool=TOOL)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break the module three ways; must go RED")
    args = ap.parse_args()
    if args.positive_control:
        red = []
        for control in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                bad = run(build(tmp, control), quiet=True)
            print(f"  {'RED  ' if bad else 'green'} {control[0]}: {bad} check(s) failing")
            red.append(bad > 0)
        ok = all(red)
        print(f"{TOOL}: positive control {'RED as it must be' if ok else 'FAILED to go red'}")
        return 0 if ok else 1
    with tempfile.TemporaryDirectory() as tmp:
        bad = run(build(tmp))
    print(f"{TOOL}: {'PASS' if not bad else 'FAIL'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
