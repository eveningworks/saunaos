#!/usr/bin/env python3
"""Check userland/libs/media/uvid_indeo3.c (Indeo Video 3) against FFmpeg, on the HOST.

WHAT IS UNDER TEST. The Indeo 3 codec row, with uvid's AVI demuxer in
front of it (`0032` chunks, keyframe flags) and uvid_draw.c's 4:1:0 path
behind it, compiled with the host gcc beside a small driver.

What it asserts:

  - BIT-EXACT: every Y, U and V sample of every frame equals FFmpeg's
    decode (`-fps_mode passthrough`, yuv410p), frame by frame by time.
    A null frame (FFmpeg emits no picture for it) must come out as the
    previous picture again.
  - exact seeks: the frame showing at a time, decoded from the keyframe
    before it, is FFmpeg's frame at that time
  - the 4:1:0 drawing path: uvid_frame_draw() of a frame at its own size
    against FFmpeg's RGB of the same frame, by PSNR (two different
    conversions, so a measured bar, not equality)
  - a SYNTHESISED stream -- every escape code, modes 0 and 10, intra and
    inter, NULL copies, a motion vector -- decodes without a refusal
  - HOSTILE INPUT: thousands of truncated and corrupted frames through the
    codec under AddressSanitizer and UBSan, after a valid prefix -- a frame
    either decodes or is refused, and nothing is read out of bounds

THE FILES ARE NOT IN THE REPOSITORY. The comparison runs on the IV32 AVIs
of a local copy of DK's "Multimedia Science" CD: `--iso` (default
$SAUNAOS_INDEO3_ISO, else ~/CodingProjects/saunaos-other/DKMMSCIE.iso;
needs bsdtar), or `--dir` with .AVI files in it. With neither it SAYS SO
and skips that half; the synthetic and fuzz halves still run.

  python3 tools/hostchecks/indeo3_hostcheck.py                 # all files
  python3 tools/hostchecks/indeo3_hostcheck.py --files 4       # the first four
  python3 tools/hostchecks/indeo3_hostcheck.py --positive-control
        # a copy of the codec with the intra requantisation's rounding
        # changed (the comparison must go RED) and a dyad index unchecked
        # (the fuzz must go RED under ASan)

Needs gcc (with ASan/UBSan) and ffmpeg.
"""

# Indeo 3 vs ffmpeg, bit-exact on local DK AVIs; synthetic stream and
# ASan/UBSan fuzz always.
RUNNER = {
    "name": "indeo3_host",
    "covers": "Indeo 3 vs ffmpeg bit-exact (local AVIs), fuzzed",
    "parallel": True,
    "needs": ["host_video", "needs gcc and ffmpeg on PATH"],
}

import argparse
import glob
import hashlib
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
MEDIA = "userland/libs/media"
DEFAULT_ISO = os.environ.get("SAUNAOS_INDEO3_ISO",
                             os.path.expanduser("~/CodingProjects/saunaos-other/DKMMSCIE.iso"))

# The ring-3 syscalls uvid uses, as POSIX underneath the same names.
SHIM_SYS_H = r"""
#ifndef HOST_SYS_H
#define HOST_SYS_H
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define SYS_SEEK_SET 0
struct sys_stat { uint64_t size; };
static inline int64_t sys_read(int fd, void *b, size_t n) { return read(fd, b, n); }
static inline int sys_open(const char *p, int f) { (void)f; return open(p, O_RDONLY); }
static inline int sys_close(int fd) { return close(fd); }
static inline long long sys_lseek(int fd, long long o, int w) { return lseek(fd, o, w); }
static inline int sys_fstat(int fd, struct sys_stat *s) {
    struct stat st; if (fstat(fd, &st)) return -1; s->size = (uint64_t)st.st_size; return 0;
}
#endif
"""

DRIVER = r"""
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "media/uvid.h"
#include "media/uvid_internal.h"
#include "media/uimg.h"

// Motion JPEG goes through media/uimg.h, judged by uimg_hostcheck; not here.
const struct uvid_codec uvid_codec_mjpeg = { .id = UVID_VC_MJPEG, .name = "mjpeg" };

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void put_frame(const struct uvid_frame *f) {
    int s = uvid_chroma_shift(f);
    int cw = (f->w + (1 << s) - 1) >> s, ch = (f->h + (1 << s) - 1) >> s;
    for (int y = 0; y < f->h; y++) fwrite(f->y + (size_t)y * f->y_stride, 1, (size_t)f->w, stdout);
    for (int y = 0; y < ch; y++) fwrite(f->cb + (size_t)y * f->c_stride, 1, (size_t)cw, stdout);
    for (int y = 0; y < ch; y++) fwrite(f->cr + (size_t)y * f->c_stride, 1, (size_t)cw, stdout);
}

static int decode(const char *path, int draw_at) {
    struct uvid *v;
    int rc = uvid_open(path, &v);
    if (rc) { fprintf(stderr, "open: %s (%d)\n", uvid_last_error(), rc); return 3; }
    const struct uvid_info *in = uvid_info(v);
    fprintf(stderr, "info %d %d %u %u %s\n", in->w, in->h, in->fps_num, in->fps_den, in->codec);
    const struct uvid_frame *f;
    int n = 0;
    double t0 = now(), spent = 0;
    while ((rc = uvid_next(v, &f)) == 1) {
        spent += now() - t0;
        if (draw_at < 0) {
            fprintf(stderr, "frame %lld %c %d\n", (long long)f->pts_ms, f->type, f->fmt);
            put_frame(f);
        } else if (n == draw_at) {
            uint32_t *px = malloc((size_t)f->w * f->h * 4);
            uvid_frame_draw(f, 0, 0, f->w, f->h, px, f->w, f->w, f->h, UVID_SMOOTH);
            for (int i = 0; i < f->w * f->h; i++) {
                uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
                fwrite(rgb, 1, 3, stdout);
            }
            free(px);
            break;
        }
        n++;
        t0 = now();
    }
    fprintf(stderr, "time %.6f frames %d\n", spent, n);
    uvid_close(v);
    return rc < 0 ? 6 : 0;
}

// The frame showing at `ms`, by an exact seek: back to a keyframe, then on.
static int seek(const char *path, uint32_t ms) {
    struct uvid *v;
    if (uvid_open(path, &v)) { fprintf(stderr, "open: %s\n", uvid_last_error()); return 3; }
    const struct uvid_frame *f;
    int rc = uvid_seek(v, ms, UVID_SEEK_EXACT);
    if (rc || uvid_next(v, &f) != 1) { fprintf(stderr, "seek: %s (%d)\n", uvid_last_error(), rc); uvid_close(v); return 4; }
    fprintf(stderr, "frame %lld %c %d\n", (long long)f->pts_ms, f->type, f->fmt);
    put_frame(f);
    uvid_close(v);
    return 0;
}

// --- hostile input ---------------------------------------------------------

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

struct pk { uint8_t *d; size_t n; };

// Feeds packets [0, upto) and then `last` to a fresh codec, as uvid does.
static int run(const struct pk *p, int upto, const uint8_t *last, size_t lastn) {
    struct uvid v;
    memset(&v, 0, sizeof v);
    if (uvid_codec_indeo3.open(&v)) return -1;
    int ok = 0;
    for (int i = 0; i <= upto; i++) {
        struct uvid_pkt q = { (uint8_t *)(i < upto ? p[i].d : last), i < upto ? p[i].n : lastn, 0, i * 66, 1 };
        int rc = uvid_codec_indeo3.feed(&v, &q);
        const struct uvid_frame *f;
        if (uvid_codec_indeo3.frame(&v, 0, &f)) {
            // Touch every sample handed out: a frame larger than its planes shows here.
            volatile unsigned sum = 0;
            int s = uvid_chroma_shift(f), cw = (f->w + 3) >> s, ch = (f->h + 3) >> s;
            for (int y = 0; y < f->h; y++) for (int x = 0; x < f->w; x++) sum += f->y[y * f->y_stride + x];
            for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) sum += f->cb[y * f->c_stride + x] + f->cr[y * f->c_stride + x];
            (void)sum;
        }
        if (i == upto) ok = rc == 0;
        if (i == upto / 2 && (rnd() & 7) == 0) uvid_codec_indeo3.reset(&v);
    }
    uvid_codec_indeo3.close(&v);
    return ok;
}

static int fuzz(const char *path, int rounds) {
    struct uvid *v;
    if (uvid_open(path, &v)) { fprintf(stderr, "open: %s\n", uvid_last_error()); return 3; }
    struct pk p[24];
    int n = 0;
    struct uvid_pkt pkt = { 0 };
    while (n < 24 && v->src.ops->read(&v->src, UVID_VIDEO, &pkt) == 1) {
        p[n].d = malloc(pkt.len ? pkt.len : 1);
        memcpy(p[n].d, pkt.data, pkt.len);
        p[n].n = pkt.len;
        n++;
    }
    uvid_pkt_free(&pkt);
    uvid_close(v);
    unsigned decoded = 0, refused = 0;
    // Every truncation of the first frame, at a stride, then mutations.
    for (size_t len = 0; n && len <= p[0].n; len += 1 + p[0].n / 600) {
        uint8_t *c = malloc(len ? len : 1);
        memcpy(c, p[0].d, len);
        if (run(p, 0, c, len) > 0) decoded++; else refused++;
        free(c);
    }
    for (int r = 0; n && r < rounds; r++) {
        int i = (int)(rnd() % (unsigned)n);
        size_t len = p[i].n;
        if (r % 4 == 3) len = rnd() % (len + 1);
        uint8_t *c = malloc(len ? len : 1);
        memcpy(c, p[i].d, len);
        int edits = 1 + (int)(rnd() % 6);
        for (int k = 0; k < edits && len > 16; k++) {
            size_t at = 16 + rnd() % (len - 16);  // mostly past the frame header's checksum
            if (rnd() & 1) c[at] ^= (uint8_t)(1u << (rnd() % 8));
            else c[at] = (uint8_t)rnd();
        }
        if (run(p, i, c, len) > 0) decoded++; else refused++;
        free(c);
    }
    printf("fuzz %u decoded %u refused\n", decoded, refused);
    for (int i = 0; i < n; i++) free(p[i].d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "decode")) return decode(argv[2], -1);
    if (!strcmp(argv[1], "draw") && argc > 3) return decode(argv[2], atoi(argv[3]));
    if (!strcmp(argv[1], "seek") && argc > 3) return seek(argv[2], (uint32_t)atoi(argv[3]));
    if (!strcmp(argv[1], "fuzz")) return fuzz(argv[2], argc > 3 ? atoi(argv[3]) : 3000);
    return 2;
}
"""

SOURCES = ["uvid.c", "uvid_avi.c", "uvid_ps.c", "uvid_mpeg1.c", "uvid_draw.c", "uvid_indeo3.c"]

# The control: the intra requantisation rounds down instead of to nearest
# (the comparison must go red), and a dyad pair's second index goes
# unchecked (the fuzz must go red: it reads past the table).
CONTROL = [
    ("int v = (2 * row[i] + m) / (2 * m) * m - q64 + IV3_PAD;",
     "int v = (2 * row[i]) / (2 * m) * m - q64 + IV3_PAD;"),
    ("if (b->bad || k2 >= t->num_dyads) return -1;", "if (b->bad) return -1;"),
]

# MEASURED on CHRE0VLP's frame 20: uvid_frame_draw's RGB against FFmpeg's
# is 40 dB (different chroma upsampling and rounding); the same frame with
# its chroma planes read as 4:2:0 is 10 dB. 32 sits between.
MIN_DRAW_PSNR = 32.0


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


# --- a synthetic stream ------------------------------------------------------

class Codes:
    """The decoder's reader in reverse: 2-bit tree codes four to a byte, MSB
    first, the byte placed where its first code is read; data bytes inline."""

    def __init__(self):
        self.out = bytearray()
        self.at = -1
        self.used = 4

    def code(self, c):
        if self.used == 4:
            self.out.append(0)
            self.at, self.used = len(self.out) - 1, 0
        self.out[self.at] |= c << (6 - 2 * self.used)
        self.used += 1

    def byte(self, b):
        self.out.append(b & 0xff)


# Tables 0 (set 1, 195 dyads, quad side 7) and 8 (set 2, the same shape).
ND, SIDE = 195, 7


def blocks(cw, n, inter, rnd):
    """`n` blocks of random but valid codes."""
    left = n
    while left > 0:
        left -= 1
        pick = rnd.random()
        if inter and pick < 0.08:
            cw.byte(0xfa)
            continue
        if inter and pick < 0.12 and left >= 1:
            cw.byte(0xf9)
            left -= 1
            continue
        if pick < 0.18 and left >= 2:
            k = rnd.randint(1, min(left + 1, 5))
            cw.byte(0xfb)
            cw.byte(k | (0x20 if inter and rnd.random() < 0.5 else 0))
            left -= k - 1
            continue
        if pick < 0.22 and left >= 1:
            cw.byte(0xfc)
            left -= 1
            continue
        line = 0
        while line < 4:
            q = rnd.random()
            if line == 0 and q < 0.1:
                cw.byte(0xff)
                line = 2
            elif line <= 1 and q < 0.2:
                cw.byte(0xfe)
                line = 3
            elif q < 0.27:
                cw.byte(0xfd)
                line = 4
            elif q < 0.6:
                cw.byte(ND + rnd.randrange(SIDE * SIDE))
                line += 1
            else:
                cw.byte(rnd.randrange(12))      # small deltas: stay in range
                cw.byte(rnd.randrange(12))
                line += 1


def plane(w, h, key, rnd):
    """One plane: two cells split top/bottom (sizes as the decoder splits)."""
    top = (h // 8 + 1) // 2 * 8
    cw = Codes()
    if key:
        cw.out += struct.pack("<I", 0)
        cw.code(0)
        cw.code(2); cw.code(3); cw.byte(0x08)               # intra, mode 0, table 8
        blocks(cw, (w // 4) * (top // 4), False, rnd)
        cw.code(2); cw.code(3); cw.byte(0xa8)               # intra, mode 10
        blocks(cw, (w // 8) * ((h - top) // 8), False, rnd)
    else:
        cw.out += struct.pack("<I", 2) + bytes([0, 0, 2, 0])  # (0,0) and two rows down
        cw.code(0)
        cw.code(3); cw.byte(1)
        cw.code(3); cw.byte(0x00)                           # inter, mode 0, table 0
        blocks(cw, (w // 4) * (top // 4), True, rnd)
        cw.code(3); cw.byte(0)
        cw.code(1)                                          # left | right, as the decoder splits
        cw.code(3); cw.byte(0xa0)                           # inter, mode 10
        blocks(cw, ((w // 8 + 1) // 2) * ((h - top) // 8), True, rnd)
        cw.code(2); cw.code(0)                              # NULL: copy
    return bytes(cw.out)


def frame(n, key, buf, w, h, rnd):
    cwid, chgt = ((w + 3) // 4 + 3) & ~3, ((h + 3) // 4 + 3) & ~3
    u, v, y = plane(cwid, chgt, key, rnd), plane(cwid, chgt, key, rnd), plane(w, h, key, rnd)
    flags = (4 | 1 if key else 0) | (0x200 if buf else 0)
    uo = 48
    vo = uo + len(u)
    yo = vo + len(v)
    body = u + v + y
    hdr = struct.pack("<HHIBBHHHIIII", 32, flags, (48 + len(body)) * 8, 0, 0, 0, h, w, yo, vo, uo, 0)
    hdr += bytes.fromhex("021426384a5c6e7f8294a6b8cadceeff")
    data = hdr + body
    fsz = len(data)
    return struct.pack("<IIII", n, 0, n ^ fsz ^ 0x46524d48, fsz) + data


def write_avi(path, frames, w, h):
    def chunk(cid, data):
        return cid + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")

    def lst(kind, data):
        return chunk(b"LIST", kind + data)
    avih = struct.pack("<IIIIIIIIII", 66666, 0, 0, 0x10, len(frames), 0, 1, 0, w, h) + bytes(16)
    strh = b"vidsIV32" + struct.pack("<IHHIIIIIIIIhhhh", 0, 0, 0, 0, 66666, 1000000, 0, len(frames), 0, 0, 0, 0, 0, w, h)
    strf = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, struct.unpack("<I", b"IV32")[0], w * h * 3, 0, 0, 0, 0)
    hdrl = lst(b"hdrl", chunk(b"avih", avih) + lst(b"strl", chunk(b"strh", strh) + chunk(b"strf", strf)))
    movi_body, idx, off = b"", b"", 4
    for i, (key, f) in enumerate(frames):
        c = chunk(b"00dc", f)
        idx += b"00dc" + struct.pack("<III", 0x10 if key else 0, off, len(f))
        movi_body += c
        off += len(c)
    body = b"AVI " + hdrl + lst(b"movi", movi_body) + chunk(b"idx1", idx)
    with open(path, "wb") as fh:
        fh.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def synthetic(path, rnd):
    w, h = 160, 120
    frames = []
    for n in range(12):
        key = n % 6 == 0
        frames.append((key, frame(n, key, n & 1, w, h, rnd)))
    write_avi(path, frames, w, h)


# --- the files --------------------------------------------------------------

def find_files(args, work):
    """(list of AVI paths, a sentence about where they came from or why none)."""
    if args.dir:
        files = sorted(glob.glob(os.path.join(args.dir, "**", "*.[Aa][Vv][Ii]"), recursive=True))
        if not files:
            return [], f"no IV32 files: no .AVI under {args.dir}"
        return files, f"{len(files)} AVI file(s) under {args.dir}"
    if not os.path.exists(args.iso):
        return [], f"no IV32 files: {args.iso} is not here (give --iso or --dir)"
    if not shutil.which("bsdtar"):
        return [], f"no IV32 files: reading {args.iso} needs bsdtar"
    out = os.path.join(work, "iso")
    os.makedirs(out, exist_ok=True)
    r = run(["bsdtar", "xf", args.iso, "-C", out, "--include", "*.AVI"], text=True)
    if r.returncode:
        return [], f"no IV32 files: bsdtar could not read {args.iso}: {r.stderr.strip()[:120]}"
    files = sorted(glob.glob(os.path.join(out, "**", "*.AVI"), recursive=True))
    return files, f"{len(files)} AVI file(s) from {args.iso}"


def ffmpeg_frames(path):
    """FFmpeg's decoded frames: [(ms, md5)], and the frame size, from framemd5."""
    r = run(["ffmpeg", "-v", "error", "-i", path, "-an", "-fps_mode", "passthrough",
             "-pix_fmt", "yuv410p", "-f", "framemd5", "-"], text=True)
    if r.returncode:
        return None, r.stderr.strip()[:200]
    tb = None
    out = []
    for line in r.stdout.splitlines():
        m = re.match(r"#tb 0: (\d+)/(\d+)", line)
        if m:
            tb = (int(m.group(1)), int(m.group(2)))
        if line.startswith("#") or not line.strip():
            continue
        f = [x.strip() for x in line.split(",")]
        if f[0] != "0":
            continue
        out.append((int(f[2]) * 1000 * tb[0] // tb[1], f[5]))
    return out, ""


def first_diff(path, ours, ms, w, h):
    """Where FFmpeg's frame at `ms` first differs from `ours`: a sentence."""
    r = run(["ffmpeg", "-v", "error", "-i", path, "-an", "-fps_mode", "passthrough",
             "-pix_fmt", "yuv410p", "-f", "rawvideo", "-"])
    cw, ch = (w + 3) // 4, (h + 3) // 4
    fs = w * h + 2 * cw * ch
    frames = [r.stdout[i:i + fs] for i in range(0, len(r.stdout), fs)]
    want = None
    for idx, (fms, _md5) in enumerate(ffmpeg_frames(path)[0]):
        if fms == ms:
            want = frames[idx]
    if want is None:
        return "FFmpeg has no frame there"
    planes = (("Y", 0, w, w * h), ("U", w * h, cw, cw * ch), ("V", w * h + cw * ch, cw, cw * ch))
    for name, off, pw, n in planes:
        for k in range(n):
            if ours[off + k] != want[off + k]:
                bad = sum(1 for j in range(n) if ours[off + j] != want[off + j])
                return "%s (%d,%d): ours %d, FFmpeg %d; %d samples of %s differ" % (
                    name, k % pw, k // pw, ours[off + k], want[off + k], bad, name)
    return "no sample differs (a length mismatch)"


def compare(exe, path):
    """(ok, detail, frames, seconds, size) for one file."""
    r = run([exe, "decode", path])
    if r.returncode:
        return False, "decoder exit %d: %s" % (r.returncode, r.stderr.decode().strip()[-200:]), 0, 0, ""
    err = r.stderr.decode().splitlines()
    info = [ln.split() for ln in err if ln.startswith("info")][0]
    w, h = int(info[1]), int(info[2])
    if info[5] != "indeo3":
        return False, "not decoded as Indeo 3 (codec %r)" % info[5], 0, 0, ""
    secs = float([ln.split()[1] for ln in err if ln.startswith("time")][0])
    size = "%dx%d" % (w, h)
    fs = w * h + 2 * ((w + 3) // 4) * ((h + 3) // 4)
    pts = [int(ln.split()[1]) for ln in err if ln.startswith("frame")]
    ours = {}
    order = []
    for i, ms in enumerate(pts):
        ours[ms] = r.stdout[i * fs:(i + 1) * fs]
        order.append(ms)
    theirs, why = ffmpeg_frames(path)
    if theirs is None:
        return False, "ffmpeg failed: " + why, 0, 0, ""
    have = {ms for ms, _ in theirs}
    for ms, md5 in theirs:
        mine = ours.get(ms)
        if mine is None:
            return False, "no frame at %d ms, which FFmpeg decodes" % ms, len(pts), secs, size
        if hashlib.md5(mine).hexdigest() != md5:
            return False, "frame at %d ms: %s" % (ms, first_diff(path, mine, ms, w, h)), len(pts), secs, size
    repeats = 0
    for i, ms in enumerate(order):
        if ms in have:
            continue
        if i == 0 or ours[ms] != ours[order[i - 1]]:
            return False, "frame at %d ms, where FFmpeg has none, is not the previous picture" % ms, len(pts), secs, size
        repeats += 1
    extra = " (+%d null frame(s) repeated)" % repeats if repeats else ""
    return True, "%d frames bit-exact%s" % (len(theirs), extra), len(pts), secs, size


def check_seeks(exe, path):
    """Exact seeks land on the frame FFmpeg shows at that time -- past a
    keyframe, so the reference chain is rebuilt from it."""
    theirs, why = ffmpeg_frames(path)
    if not theirs:
        return False, "ffmpeg failed: " + why
    last = theirs[-1][0]
    picked = []
    for ms in (0, 1000, 2520, last // 2 + 37, max(0, last - 5)):
        want = [md5 for t, md5 in theirs if t <= ms][-1]
        r = run([exe, "seek", path, str(ms)])
        if r.returncode:
            return False, "seek to %d ms: %s" % (ms, r.stderr.decode().strip()[-120:])
        if hashlib.md5(r.stdout).hexdigest() != want:
            return False, "seek to %d ms gave a different picture from FFmpeg's" % ms
        picked.append(ms)
    return True, "exact seeks to %s ms" % ", ".join(map(str, picked))


def psnr(a, b):
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    return float("inf") if se == 0 else 10 * math.log10(255 * 255 * len(a) / se)


def check_draw(exe, path, at):
    info = run([exe, "decode", path]).stderr.decode().splitlines()
    w, h = map(int, [ln.split() for ln in info if ln.startswith("info")][0][1:3])
    ours = run([exe, "draw", path, str(at)]).stdout
    ref = run(["ffmpeg", "-v", "error", "-i", path, "-an", "-fps_mode", "passthrough", "-vf",
               "select=eq(n\\,%d)" % at, "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]).stdout
    if len(ours) != w * h * 3 or len(ref) != w * h * 3:
        return False, "sizes: ours %d, FFmpeg %d bytes" % (len(ours), len(ref))
    p = psnr(ours, ref)
    return p >= MIN_DRAW_PSNR, "frame %d RGB PSNR %.1f dB (bar %.0f)" % (at, p, MIN_DRAW_PSNR)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--iso", default=DEFAULT_ISO, help="the CD image to take IV32 AVIs from")
    ap.add_argument("--dir", help="a directory of IV32 AVIs instead")
    ap.add_argument("--files", type=int, default=0, help="compare only the first N files")
    ap.add_argument("--rounds", type=int, default=3000, help="fuzz mutations per seed")
    ap.add_argument("--keep", help="keep the work directory here")
    ap.add_argument("--positive-control", action="store_true",
                    help="break a copy of the codec twice; the check must go RED")
    args = ap.parse_args()

    work = args.keep or tempfile.mkdtemp(prefix="indeo3check.")
    os.makedirs(work, exist_ok=True)
    hostcheck.write(work, "inc/rt/sys.h", SHIM_SYS_H)
    drv = hostcheck.write(work, "driver.c", DRIVER)
    srcs = [hostcheck.stage(work, f"{MEDIA}/{s}", dest="src/" + s,
                            edits=CONTROL if s == "uvid_indeo3.c" else (),
                            apply=args.positive_control and s == "uvid_indeo3.c", tool="indeo3_hostcheck")
            for s in SOURCES] + [drv]
    incs = [os.path.join(work, "inc"), os.path.join(ROOT, "userland"), os.path.join(ROOT, "userland", "libs"),
            os.path.join(ROOT, "userland", "third_party", "indeo3", "upstream"),
            os.path.join(ROOT, "userland", "third_party", "indeo3", "glue")]
    common = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-include", "string.h"]
    exe = hostcheck.compile(work, "indeo3_host", srcs, flags=["-O2", *common], includes=incs,
                            tool="indeo3_hostcheck")
    asan = hostcheck.compile(work, "indeo3_asan", srcs,
                             flags=["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                    *common], includes=incs, tool="indeo3_hostcheck")

    fails = []
    red = {"compare": False, "fuzz": False}

    def report(ok, label, detail, kind=None):
        print("  %-5s %-30s %s" % ("ok" if ok else "FAIL", label, detail))
        if not ok:
            fails.append(label)
            if kind:
                red[kind] = True

    # The synthetic stream: valid by construction, so nothing may be refused.
    syn = os.path.join(work, "synthetic.avi")
    synthetic(syn, random.Random(1993))
    r = run([exe, "decode", syn])
    n = sum(1 for ln in r.stderr.decode().splitlines() if ln.startswith("frame"))
    report(r.returncode == 0 and n == 12, "synthetic stream", "%d of 12 frames decoded" % n)

    files, where = find_files(args, work)
    if args.files and len(files) > args.files:
        files = files[:args.files]
        where += ", the first %d compared" % args.files
    seeds = [syn] + files[:2]
    if not files:
        print("  skip  %-30s %s" % ("bit-exact vs FFmpeg", where))
    else:
        print("  ..    comparing %s" % where)
        speed = {}
        for path in files:
            ok, detail, frames, secs, size = compare(exe, path)
            report(ok, os.path.basename(path), detail, "compare")
            if frames:
                t, k = speed.get(size, (0.0, 0))
                speed[size] = (t + secs, k + frames)
        for size, (t, k) in sorted(speed.items()):
            print("  ..    decode speed %-12s %.2f ms a frame (host, -O2, %d frames)" % (size, 1000 * t / k, k))
        ok, detail = check_seeks(exe, files[0])
        report(ok, "seeking", detail, "compare")
        ok, detail = check_draw(exe, files[0], 20)
        report(ok, "4:1:0 drawn as RGB", detail, "compare")

    for seed in seeds:
        fz = run([asan, "fuzz", seed, str(args.rounds)], text=True)
        label = "hostile " + os.path.basename(seed)
        if fz.returncode:
            why = [ln for ln in fz.stderr.splitlines() if "ERROR:" in ln or "runtime error" in ln]
            report(False, label, why[0].strip() if why else "exit %d" % fz.returncode, "fuzz")
            if not args.positive_control:
                print(fz.stderr[-3000:])
        else:
            report(True, label, fz.stdout.strip() + ", no fault")

    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    if args.positive_control:
        cmp_state = "RED" if red["compare"] else ("skipped, no files" if not files else "green")
        print("indeo3_hostcheck: positive control: comparison %s, fuzz %s"
              % (cmp_state, "RED" if red["fuzz"] else "green"))
        return 0 if red["fuzz"] and (red["compare"] or not files) else 1
    print("indeo3_hostcheck: %s" % ("FAILED" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
