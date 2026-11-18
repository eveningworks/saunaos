#!/usr/bin/env python3
"""Grapheme cluster boundaries (kernel/lib/kgrapheme.c) against Perl's `\\X`, on the HOST.

`kgrapheme_next()` and `kgrapheme_prev()` split UTF-8 into UAX #29 extended
grapheme clusters -- the unit the caret steps over, the terminal gives its
cells and a measurement counts. This compiles the tree's kgrapheme.c with the
host gcc and checks both directions against PERL's regex `\\X`, an independent
implementation of the same annex, over:

  - every sequence in tools/gen/data/emoji-test.txt, between letters
  - letters with combining marks (Latin, Greek, Cyrillic), regional
    indicator runs of 1 to 5, Hangul jamo and syllables, a Prepend
  - 3,000 random strings drawn from a pool holding every class

A tab stands in for a line break (a control, the same rules), since the
cases travel one per line. Kept OUT of the inputs, as kgrapheme.h documents: CR (its GB3 "\\r\\n" is
deliberately two characters here) and Indic conjunct linkers (GB9c is not
applied). Also checks kgrapheme_backspace() at its cases.

`--positive-control` drops the regional-indicator pairing (GB12/13) and,
separately, GB11's ZWJ join; each must go RED.
"""

# kgrapheme.c's cluster boundaries vs Perl's \X, both directions. Host gcc and
# perl. Carries a --positive-control that must go red.
RUNNER = {"name": "grapheme_host", "covers": "UAX #29 grapheme clusters (kgrapheme.c) vs Perl's \\X, forward and back",
          "gate": True, "parallel": True}

import argparse
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = toolpath.REPO
TOOL = "grapheme_hostcheck"
EMOJI_TEST = os.path.join(ROOT, "tools", "gen", "data", "emoji-test.txt")

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "kgrapheme.h"

static char line[1 << 16];

int main(void) {
    while (fgets(line, sizeof line, stdin)) {
        int n = (int)strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (!strcmp(line, "#BS")) {   /* backspace cases follow, one per line */
            while (fgets(line, sizeof line, stdin)) {
                n = (int)strlen(line);
                if (n && line[n - 1] == '\n') line[--n] = 0;
                printf("%d\n", kgrapheme_backspace(line, n));
            }
            return 0;
        }
        for (int i = 0, j; (j = kgrapheme_next(line, -1, i)) > i; i = j) printf("%d ", j);
        printf("|");
        for (int i = n, j; i > 0; i = j) {
            j = kgrapheme_prev(line, i);
            printf(" %d", i);
            if (j >= i) break;
        }
        printf("\n");
    }
    return 0;
}
"""

PERL = r"""
binmode STDIN, ':encoding(UTF-8)';
while (my $l = <STDIN>) {
    chomp $l;
    my @b; my $off = 0;
    for my $g ($l =~ /(\X)/g) { utf8::encode(my $e = $g); $off += length $e; push @b, $off; }
    print join(' ', @b), "\n";
}
"""

CONTROLS = [
    ("regional indicators never pair",
     "    if (p == KGB_RI && k == KGB_RI && (w->ri & 1)) return 1;                  // GB12, GB13",
     "    if (p == KGB_RI && k == KGB_RI && 0) return 1;"),
    ("ZWJ joins no pictograph",
     "    if (p == KGB_ZWJ && k == KGB_EP && w->ep_zwj) return 1;                   // GB11",
     "    if (p == KGB_ZWJ && k == KGB_EP && 0) return 1;"),
]

# (text, the byte offset Backspace at the end must delete back to)
BACKSPACE = [
    ("ab", 1),
    ("e\u0301", 1),                                         # a mark goes alone
    ("x\U0001F468\u200D\U0001F469\u200D\U0001F467", 1),     # a family goes whole
    ("x\U0001F1EB\U0001F1EE", 1),                           # a flag
    ("x\U0001F44D\U0001F3FD", 1),                           # a skin tone
    ("x1\uFE0F\u20E3", 1),                                  # a keycap
    ("x\u2764\uFE0F", 1),                                   # a selector stays with its base
]


def emoji_sequences():
    out = []
    for raw in open(EMOJI_TEST, encoding="utf-8"):
        if raw.startswith("#") or ";" not in raw:
            continue
        out.append("".join(chr(int(c, 16)) for c in raw.split(";")[0].split()))
    return out


def corpus():
    cases = [f"a{s}b" for s in emoji_sequences()]
    marks = ["\u0301", "\u0308", "\u0327", "\u0342", "\u0483", "\u20E3", "\uFE0F", "\uFE0E", "\u200D"]
    for base in "aeE\u0391\u03B1\u03A9\u0416\u04361 ":
        for m1 in marks:
            cases.append(f"x{base}{m1}y")
            for m2 in marks:
                cases.append(f"{base}{m1}{m2}z")
    ri = [chr(0x1F1E6 + i) for i in range(26)]
    for n in range(1, 6):
        cases.append("a" + "".join(ri[(i * 7) % 26] for i in range(n)) + "b")
        cases.append("".join(ri[(i * 3) % 26] for i in range(n)) + "\u0301")
    cases += ["\u1100\u1161\u11A8x", "\uAC00\u11A8", "\uAC01\u11A8\u11A8", "\u1100\uAC00", "\u0600\u0661",
              "a\tb\u0301", "\t\uFE0F\uFE0F", "\uFE0F\uFE0Fa", "\u200D\U0001F468", "\U0001F468\u0301\u200D\U0001F469"]
    pool = (list("aZ09 .,") + ["\u00E9", "\u03B1", "\u0436", "\u0301", "\u0308", "\u0483", "\u200D", "\uFE0F",
            "\uFE0E", "\U0001F468", "\U0001F469", "\u2764", "\U0001F3FD", "\U0001F1EB", "\U0001F1EE", "\u20E3",
            "\U000E0067", "\U000E007F", "\U0001F3F4", "\u1100", "\u1161", "\u11A8", "\uAC00", "\uAC01", "\u0600",
            "\u0903", "\u00AD", "\u200B", "\t", "\u2122", "\u00A9"])
    rnd = random.Random(29)
    for _ in range(3000):
        cases.append("".join(rnd.choice(pool) for _ in range(rnd.randint(1, 14))))
    return cases


def build(tmp, control=None):
    inc = os.path.join(tmp, "inc")
    os.makedirs(inc, exist_ok=True)
    for h in ("kernel/include/api/kgrapheme.h", "kernel/include/api/kgrapheme_table.h",
              "kernel/include/api/kutf8.h"):
        hostcheck.stage(inc, h, tool=TOOL)
    edits = [(control[1], control[2])] if control else []
    src = hostcheck.stage(tmp, "kernel/lib/kgrapheme.c", edits=edits, apply=bool(edits), tool=TOOL)
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "grapheme_check", [drv, src], includes=[inc], tool=TOOL)


def run(exe, quiet):
    cases = corpus()
    text = "\n".join(cases) + "\n"
    ours = subprocess.run([exe], input=text.encode(), capture_output=True, check=True).stdout.decode().splitlines()
    ref = subprocess.run(["perl", "-e", PERL], input=text.encode(), capture_output=True, check=True).stdout.decode().splitlines()
    fwd_bad, back_bad = [], []
    for case, o, r in zip(cases, ours, ref):
        fwd, back = o.split("|")
        if fwd.split() != r.split():
            fwd_bad.append(case)
        if sorted(int(x) for x in back.split()) != [int(x) for x in r.split()]:
            back_bad.append(case)
    bs = subprocess.run([exe], input=("#BS\n" + "\n".join(t for t, _ in BACKSPACE) + "\n").encode(),
                        capture_output=True, check=True).stdout.decode().split()
    bs_bad = [(t, int(g), w) for (t, w), g in zip(BACKSPACE, bs) if int(g) != w]
    bad = 0
    for label, wrong, total in (("forward (kgrapheme_next)", fwd_bad, len(cases)),
                                ("backward (kgrapheme_prev)", back_bad, len(cases))):
        ok = not wrong and len(ours) == len(cases) == len(ref)
        bad += not ok
        if not quiet:
            print(f"  {'ok  ' if ok else 'FAIL'} {label}: {total - len(wrong)}/{total} strings split as Perl's \\X"
                  + (f" -- first {[ascii(w) for w in wrong[:3]]}" if wrong else ""))
    ok = not bs_bad and len(bs) == len(BACKSPACE)
    bad += not ok
    if not quiet:
        print(f"  {'ok  ' if ok else 'FAIL'} kgrapheme_backspace(): {len(BACKSPACE) - len(bs_bad)}/{len(BACKSPACE)} cases"
              + (f" -- {[(ascii(t), g, w) for t, g, w in bs_bad]}" if bs_bad else ""))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break two rules; must go RED")
    args = ap.parse_args()
    if args.positive_control:
        red = []
        for control in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                bad = run(build(tmp, control), True)
            print(f"  {'RED  ' if bad else 'green'} {control[0]}: {bad} check(s) failing")
            red.append(bad > 0)
        ok = all(red)
        print(f"{TOOL}: positive control {'RED as it must be' if ok else 'FAILED to go red'}")
        return 0 if ok else 1
    with tempfile.TemporaryDirectory() as tmp:
        bad = run(build(tmp), False)
    print(f"{TOOL}: {'PASS' if not bad else f'FAIL ({bad})'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
