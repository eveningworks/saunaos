#!/usr/bin/env python3
"""The emoji list's parser, search and recents (base/uemojidata.c), on the HOST.

`uemojidata.c` reads /usr/share/unicode/emoji -- the shipped
data/usr/share/unicode/emoji -- into groups and entries, searches names, and
keeps the recently used row as one settings value. This compiles the tree's
uemojidata.c with the host gcc (the file and settings calls stubbed) and
compares it with this script's own reading of the same file, which shares no
code with it:

  - every entry: its characters (with U+FE0F where the file says), its group
    and its name; every group's name, tab word, first entry and count;
  - searches by substring of the name with the case changed ("HEART", "Cat");
  - the recents value: a pick moved to the front, a duplicate dropped, the
    row cut at 16 and at the settings ABI's 63 bytes on a character boundary,
    and a value decoded back to entries (an unknown character skipped).

`--positive-control` breaks three things in a copy -- a case-sensitive
search, recents that keep duplicates, a parse that drops U+FE0F -- and each
must go RED.

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
           "\U0001F600\U0001F600"]
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
    }
    // Queries, one per line on stdin, then recents cases.
    char line[256];
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
            int n = uemojidata_recent_push(was, (unsigned)strtoul(sp + 1, 0, 16), now, sizeof now);
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
     "if (c != cp && c != 0xFE0F && c != 0xFE0E) {",
     "if (c != 0xFE0F && c != 0xFE0E) {"),
    ("U+FE0F dropped by the parse",
     "e->vs16 = sel == 0xFE0F;",
     "e->vs16 = 0;"),
]


def oracle():
    """This script's own reading of the file: groups, entries."""
    groups, entries = [], []
    for raw in open(DATA, encoding="utf-8"):
        raw = raw.rstrip("\n")
        if raw.startswith("@"):
            name = raw[1:]
            groups.append([name, name.split(" ")[0][:15], len(entries), 0])
        elif raw and not raw.startswith("#") and groups:
            cps, name = raw.split(";", 1)
            cps = [int(x, 16) for x in cps.split()]
            entries.append((cps[0], int(len(cps) == 2), len(groups) - 1, name,
                            "".join(chr(c) for c in cps).encode().hex().upper()))
            groups[-1][3] += 1
    return groups, entries


def push(value, cp):
    """The recents rule, written again: front, no duplicates, 16 and 63 bytes."""
    out = [chr(cp)]
    for ch in value:
        if ord(ch) in (cp, 0xFE0F, 0xFE0E) or len(out) >= RECENT_MAX:
            continue
        if len("".join(out + [ch]).encode()) > VALUE_MAX - 1:
            break
        out.append(ch)
    return "".join(out)


def cases(entries):
    smile, heart, party, cat = 0x1F600, 0x2764, 0x1F389, 0x1F431
    many = "".join(chr(e[0]) for e in entries if e[0] > 0xFFFF)[:20]
    return [
        ("", smile),
        (chr(party) + chr(smile), smile),                  # a repeat moves to the front, once
        (chr(heart) + "️" + chr(cat), party),         # a stray selector is dropped
        (many, heart),                                     # cut at 16 and at 63 bytes
        (many[:15], smile),
        ("abc", cat),                                      # not emoji: kept as written, decoded away
    ]


def run(exe, quiet=False):
    groups, entries = oracle()
    pushes = cases(entries)
    stdin = "".join(f"Q\t{q}\n" for q in QUERIES)
    stdin += "".join(f"P\t{v.encode().hex()}\t{cp:X}\n" for v, cp in pushes)
    decode_in = chr(0x1F431) + "z" + chr(0x2764) + chr(0x1F600)
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
                           f"({sum(e[1] for e in entries)} with the selector)")
    got_q = [ln for ln in lines if ln.startswith("Q\t")]
    for q, line in zip(QUERIES, got_q):
        one = [i for i, e in enumerate(entries) if q in (chr(e[0]), chr(e[0]) + "\ufe0f")]
        want = one[:1] or [i for i, e in enumerate(entries) if q.lower() in e[3].lower()]
        parts = line.split("\t")[1].split()
        check(int(parts[0]) == len(want) and list(map(int, parts[1:])) == want,
              f"search {q!r}: {len(want)} match(es)")
    got_p = [ln.split("\t") for ln in lines if ln.startswith("P\t")]
    for (v, cp), line in zip(pushes, got_p):
        want = push(v, cp)
        got = bytes.fromhex(line[2]).decode("utf-8", errors="replace") if len(line) > 2 else ""
        check(got == want and int(line[1]) == len(want.encode()),
              f"recents: U+{cp:X} onto {len(v)} character(s) -> {len(want)} kept, {len(want.encode())} bytes")
    d = [ln for ln in lines if ln.startswith("D\t")]
    find = {e[0]: i for i, e in reversed(list(enumerate(entries)))}
    want_d = [find[ord(c)] for c in decode_in if ord(c) in find]
    check(bool(d) and list(map(int, d[0].split("\t")[1].split()[1:])) == want_d,
          "a recents value decodes to its entries, an unknown character skipped")
    return bad


def build(tmp, control=None):
    inc = os.path.join(tmp, "inc")
    for rel, body in STUBS.items():
        hostcheck.write(inc, rel, body)
    hostcheck.stage(inc, "userland/libs/base/uemojidata.h", dest="base/uemojidata.h", tool=TOOL)
    hostcheck.stage(inc, "userland/libs/base/uutf8.h", dest="base/uutf8.h", tool=TOOL)
    hostcheck.stage(inc, "kernel/include/api/kutf8.h", tool=TOOL)
    edits = [(control[1], control[2])] if control else []
    src = hostcheck.stage(tmp, "userland/libs/base/uemojidata.c", edits=edits, apply=bool(edits), tool=TOOL)
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "uemojidata_check", [drv, src], flags=["-O2", "-Wall", "-Wextra"],
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
