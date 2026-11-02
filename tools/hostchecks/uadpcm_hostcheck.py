#!/usr/bin/env python3
"""Check userland/lib/uadpcm.c (MS and IMA ADPCM) against ffmpeg, on the HOST.

WHAT IS UNDER TEST. The block decoder w16's waveOut plays a Windows 3.x
program's compressed sounds through (DK's encyclopaedias ship hundreds of
MS-ADPCM WAVs), compiled with the host gcc.

THE ORACLE IS FFMPEG, AND THE COMPARISON IS EXACT. Both formats are
integer algorithms with one right answer per nibble, so every sample of
uadpcm's decode must equal ffmpeg's (adpcm_ms, adpcm_ima_wav). ffmpeg
also ENCODES the vectors, from PCM this script makes: a sweep, a chord,
noise and silence -- mono and stereo, at 11025, 22050 and 44100 Hz, in
several block sizes -- so the adaptation runs through small and large
steps and the clamps.

POSITIVE CONTROL (--positive-control): in a COPY of uadpcm.c, MS ADPCM's
adaptation table loses an entry's value and IMA's step skips its 1/8
term; each must go red.

  python3 tools/hostchecks/uadpcm_hostcheck.py
  python3 tools/hostchecks/uadpcm_hostcheck.py --positive-control

Needs gcc and ffmpeg; without ffmpeg it says SKIPPED and claims no pass.
"""

# lib/uadpcm's MS and IMA ADPCM, every sample against ffmpeg's decode.
RUNNER = {"name": "uadpcm_host", "covers": "MS and IMA ADPCM exact vs ffmpeg", "parallel": True}

import argparse
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

SRC = "userland/lib/uadpcm.c"
CONTROLS = [
    ("MS adaptation", "{ 230, 230, 230, 230, 307, 409,", "{ 230, 230, 230, 230, 307, 400,"),
    ("IMA step", "int step = IMA_STEP[index[c]], diff = step >> 3;", "int step = IMA_STEP[index[c]], diff = 0;"),
]

# The driver: a WAV's fmt chunk to uadpcm_init(), its data chunk block by
# block to uadpcm_block(), the PCM to stdout as s16le.
DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/uadpcm.h"
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : 0;
    if (!f) return 2;
    static uint8_t b[64 << 20];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    const uint8_t *fmt = 0, *data = 0;
    uint32_t flen = 0, dlen = 0;
    for (size_t at = 12; at + 8 <= n;) {
        uint32_t sz = le32(b + at + 4);
        if (sz > n - at - 8) sz = (uint32_t)(n - at - 8);
        if (!memcmp(b + at, "fmt ", 4)) { fmt = b + at + 8; flen = sz; }
        if (!memcmp(b + at, "data", 4)) { data = b + at + 8; dlen = sz; }
        at += 8 + sz + (sz & 1);
    }
    static struct uadpcm a;
    if (!fmt || !data || uadpcm_init(&a, fmt, flen) != 0) { fprintf(stderr, "refused\n"); return 3; }
    int16_t *out = malloc((size_t)a.frames_per_block * a.channels * 2);
    for (uint32_t at = 0; at < dlen; at += a.block_align) {
        uint32_t len = dlen - at < a.block_align ? dlen - at : a.block_align;
        size_t fr = uadpcm_block(&a, data + at, len, out);
        fwrite(out, 2 * a.channels, fr, stdout);
    }
    return 0;
}
"""


def pcm(kind, rate, ch, seconds, rng):
    """s16le PCM of one kind of signal."""
    n = int(rate * seconds)
    out = bytearray()
    for i in range(n):
        t = i / rate
        for c in range(ch):
            if kind == "sweep":
                v = math.sin(2 * math.pi * (100 + 4000 * t / seconds) * t + c) * 30000
            elif kind == "chord":
                v = sum(math.sin(2 * math.pi * f * t) for f in (220, 277, 330, 440 * (c + 1))) * 8000
            elif kind == "noise":
                v = rng.uniform(-32768, 32767)
            else:
                v = 0 if i < n // 2 else (32767 if (i // 37) % 2 else -32768)   # silence, then a square
            out += struct.pack("<h", max(-32768, min(32767, int(v))))
    return bytes(out)


def check(exe, tmp, rng, verbose=False):
    cases = bad = 0
    for codec in ("adpcm_ms", "adpcm_ima_wav"):
        for rate in (11025, 22050, 44100):
            for ch in (1, 2):
                for kind in ("sweep", "chord", "noise", "square"):
                    for block in (256, 512, 1024, 2048):
                        raw = os.path.join(tmp, "in.raw")
                        with open(raw, "wb") as fh:
                            fh.write(pcm(kind, rate, ch, 0.7, rng))
                        wav = os.path.join(tmp, "x.wav")
                        enc = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(rate),
                                              "-ac", str(ch), "-i", raw, "-c:a", codec, "-block_size", str(block),
                                              wav], capture_output=True, text=True)
                        if enc.returncode:
                            continue                 # a block size this encoder refuses at this rate
                        ref = subprocess.run(["ffmpeg", "-v", "error", "-i", wav, "-f", "s16le", "-"],
                                             capture_output=True).stdout
                        got = subprocess.run([exe, wav], capture_output=True)
                        cases += 1
                        if got.returncode or got.stdout != ref:
                            bad += 1
                            a, b = got.stdout, ref
                            first = next((i for i in range(0, min(len(a), len(b)), 2) if a[i:i + 2] != b[i:i + 2]),
                                         min(len(a), len(b)))
                            if verbose or bad <= 5:
                                print(f"  FAIL {codec} {rate} Hz {ch} ch {kind} block {block}: {len(a)} bytes vs "
                                      f"{len(b)}, first difference at sample {first // 2}"
                                      + (f" ({got.stderr.decode().strip()})" if got.returncode else ""))
    return cases, bad


def build(tmp, edits=()):
    hostcheck.write(tmp, "drv.c", DRIVER)
    os.makedirs(os.path.join(tmp, "lib"), exist_ok=True)
    hostcheck.stage(tmp, "userland/lib/uadpcm.h", dest="lib/uadpcm.h")
    src = hostcheck.stage(tmp, SRC, edits=edits, apply=bool(edits), tool="uadpcm_hostcheck")
    return hostcheck.compile(tmp, "uadpcm_check", [os.path.join(tmp, "drv.c"), src], includes=[tmp],
                             tool="uadpcm_hostcheck")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break the decoder; the check must go RED")
    args = ap.parse_args()
    if not shutil.which("ffmpeg"):
        print("uadpcm_hostcheck: SKIPPED -- no ffmpeg on this host")
        return 0
    if args.positive_control:
        red = []
        for name, before, after in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                cases, bad = check(build(tmp, [(before, after)]), tmp, random.Random(7))
                print(f"  {'RED  ' if bad else 'green'} {name}: {bad} of {cases} differ")
                red.append(bad > 0)
        print(f"uadpcm_hostcheck: positive control {'RED as it must be' if all(red) else 'FAILED to go red'}")
        return 0 if all(red) else 1
    with tempfile.TemporaryDirectory() as tmp:
        cases, bad = check(build(tmp), tmp, random.Random(1))
    print(f"uadpcm_hostcheck: {cases} files, {bad} differ -- {'FAILED' if bad else 'PASS'}")
    return 1 if bad or not cases else 0


if __name__ == "__main__":
    sys.exit(main())
