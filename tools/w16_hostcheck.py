#!/usr/bin/env python3
"""Check w16's NE parser against a second reader, and against hostile files.

WHAT IS UNDER TEST. userland/w16/ne.c and ne_info.c -- the parser `w16
--info` and (later) the loader read a Windows 3.x program with -- compiled
with the host gcc beside a small driver.

What it asserts:

  - for every sample in userland/w16/samples/, built by tools/gen_ne.py,
    `w16 --info`'s lines EQUAL gen_ne.py --dump's, the import names in
    brackets aside: two independent readings of the same bytes
  - the names: hello's imports resolve to InitTask, CreateWindow and
    TextOut (userland/w16/w16_names.c, generated from Wine's tables)
  - HOSTILE FILES: thousands of mutated and truncated copies of each
    sample through ne_parse, every relocation, entry and module name,
    under AddressSanitizer and UBSan -- a parse either succeeds or
    refuses, and never reads outside the image

POSITIVE CONTROL (--positive-control): two edits to a COPY of ne.c. The
segment check stops bounding a segment by the file (the fuzz must go red
under ASan), and the relocation reader takes the site from the wrong
word (the comparison must go red).

Needs gcc with ASan/UBSan and nasm.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
SAMPLES = sorted(glob.glob(os.path.join(ROOT, "userland", "w16", "samples", "*.asm")))

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16/w16_internal.h"

static uint8_t *slurp(const char *p, uint32_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz ? sz : 1);
    if (fread(b, 1, sz, f) != (size_t)sz) exit(2);
    fclose(f); *n = (uint32_t)sz; return b;
}

// Every read the parser offers, so a missing bound shows under ASan.
static void walk(const struct ne_module *m) {
    char name[64];
    for (unsigned i = 1; i <= m->nmod; i++) ne_module_name(m, i, name, sizeof name);
    for (unsigned s = 1; s <= m->nseg; s++)
        for (unsigned i = 0; i < m->seg[s - 1].nrelocs; i++) {
            struct ne_reloc r; ne_reloc_at(m, s, i, &r);
        }
    for (unsigned o = 0; o < 300; o++) { uint16_t sg, of; ne_entry(m, o, &sg, &of); }
    // The segments' bytes, as the loader copies them.
    volatile unsigned sum = 0;
    for (unsigned s = 0; s < m->nseg; s++)
        if (m->seg[s].file_off)
            for (uint32_t k = 0; k < m->seg[s].length; k++) sum += m->img[m->seg[s].file_off + k];
    (void)sum;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    uint32_t n; uint8_t *img = slurp(argv[2], &n);
    static struct ne_module m;
    if (!strcmp(argv[1], "info")) {
        int e = ne_parse(img, n, &m);
        if (e < 0) { printf("refused: %s\n", ne_strerror(e)); free(img); return 1; }
        ne_print_info(&m);
        free(img);
        return 0;
    }
    // fuzz: flips and overwrites in an exact-size copy, then truncations
    unsigned ok = 0, refused = 0;
    for (int round = 0; round < 4000; round++) {
        uint32_t len = n;
        if (round % 5 == 4) len = rnd() % (n + 1);
        uint8_t *copy = malloc(len ? len : 1);
        memcpy(copy, img, len);
        int edits = 1 + rnd() % 8;
        for (int k = 0; k < edits && len; k++) {
            uint32_t at = rnd() % len;
            if (rnd() & 1) copy[at] ^= (uint8_t)(1u << (rnd() % 8));
            else copy[at] = (uint8_t)rnd();
        }
        if (ne_parse(copy, len, &m) == 0) { walk(&m); ok++; } else refused++;
        free(copy);
    }
    printf("fuzz %u parsed %u refused\n", ok, refused);
    free(img);
    return 0;
}
"""

# The control: no bound on a segment's data, and the site read one word late.
CONTROL = [
    ("if (s->file_off && !span(m, s->file_off, s->length)) return NE_E_SEGMENTS;", ""),
    ("out->site = rd16(m, r + 2);", "out->site = rd16(m, r + 4);"),
]


def build_samples(tmp):
    exes = []
    for src in SAMPLES:
        exe = os.path.join(tmp, os.path.basename(src)[:-4] + ".exe")
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_ne.py"), src, "-o", exe],
                       check=True)
        exes.append(exe)
    return exes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break two checks in a copy of ne.c; the check must go RED")
    args = ap.parse_args()
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        exes = build_samples(tmp)
        os.makedirs(os.path.join(tmp, "w16"))
        srcs = [hostcheck.write(tmp, "driver.c", DRIVER),
                hostcheck.stage(tmp, "userland/w16/ne.c", dest="w16/ne.c", edits=CONTROL,
                                apply=args.positive_control, tool="w16_hostcheck"),
                hostcheck.stage(tmp, "userland/w16/ne_info.c", dest="w16/ne_info.c"),
                hostcheck.stage(tmp, "userland/w16/w16_names.c", dest="w16/w16_names.c")]
        for h in ("ne.h", "w16_internal.h", "w16_names.h", "cpu16.h"):
            hostcheck.stage(tmp, f"userland/w16/{h}", dest=f"w16/{h}")
        flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-I" + tmp, "-I" + os.path.join(tmp, "w16")]
        exe = hostcheck.compile(tmp, "w16_host", srcs, flags=flags, tool="w16_hostcheck")

        for ne in exes:
            name = os.path.basename(ne)
            info = subprocess.run([exe, "info", ne], capture_output=True, text=True)
            dump = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_ne.py"),
                                   "--dump", ne], capture_output=True, text=True, check=True).stdout
            ours = re.sub(r" \([^)]*\)$", "", info.stdout, flags=re.M)
            if info.returncode or ours != dump:
                fails.append(f"{name}: w16 --info differs from gen_ne.py --dump")
                if not args.positive_control:
                    import difflib
                    print("".join(difflib.unified_diff(dump.splitlines(True), ours.splitlines(True),
                                                       "gen_ne --dump", "w16 --info")))
            else:
                print(f"  ok  {name}: the two readers agree ({len(dump.splitlines())} lines)")
            if name == "hello.exe":
                want = ["KERNEL.91 (InitTask)", "USER.41 (CreateWindow)", "GDI.33 (TextOut)"]
                missing = [w for w in want if w not in info.stdout]
                if missing:
                    fails.append(f"hello.exe: imports not named: {missing}")
                else:
                    print("  ok  hello.exe: imports are named (InitTask, CreateWindow, TextOut)")
            fz = subprocess.run([exe, "fuzz", ne], capture_output=True, text=True)
            if fz.returncode:
                fails.append(f"{name}: the parser faulted on a mutated file")
                why = [ln for ln in fz.stderr.splitlines() if "ERROR:" in ln or "runtime error" in ln]
                print(f"  FAIL {name}: " + (why[0].strip() if why else f"exit {fz.returncode}"))
                if not args.positive_control:
                    print(fz.stderr[-3000:])
            else:
                print(f"  ok  {name}: {fz.stdout.strip()}, no fault")

    if args.positive_control:
        red = ("differs" in " ".join(fails), "faulted" in " ".join(fails))
        print(f"w16_hostcheck: positive control: comparison {'RED' if red[0] else 'green'}, "
              f"fuzz {'RED' if red[1] else 'green'}")
        return 0 if all(red) else 1
    for f in fails:
        print("  FAIL " + f)
    print(f"w16_hostcheck: {'FAILED' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
