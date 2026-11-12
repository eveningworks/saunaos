#!/usr/bin/env python3
"""The kernel's USB device-reply parsers against a hostile device, on the host.

WHAT IS UNDER TEST. kernel/lib/usb_audio_parse.c (the UAC descriptor
walk, also ring 3's usbaudio) and kernel/lib/scsi.c (READ CAPACITY),
compiled with the host gcc under AddressSanitizer and UBSan beside
mediafuzz/descfuzz.c. A device answers whatever it likes, so each
parser's ACCEPTED answer is held to what its caller then assumes:

  - CRAFTED REPLIES, one per audit finding (2026-11-12): five playable
    UAC formats, the default on the fifth (the bind divided by a zero
    subslot -- a kernel panic on plug-in), and capacities whose sector
    count wraps 64 bits
  - A PLAIN STREAM, one 16-bit format, which must still be ACCEPTED: a
    parse that refuses everything passes every hostile case
  - MUTATIONS of all of them, every one parsed or refused with nothing
    read outside its buffer and no contract broken

POSITIVE CONTROL (--positive-control): the parse may take a default it
did not store and accept a stream left with no format (the fix is both),
and the capacity check is dropped -- the crafted cases must go red.

    python3 tools/hostchecks/usbdesc_hostcheck.py [--positive-control] [--iters N]
"""

# USB Audio Class descriptors and SCSI capacities from a hostile device,
# under ASan + UBSan. ~10 s.
RUNNER = {"covers": "USB descriptors and SCSI replies: crafted, real, mutated", "gate": True}

import argparse
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

ROOT = hostcheck.ROOT
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mediafuzz")
TOOL = "usbdesc_hostcheck"
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")
INC = [os.path.join(ROOT, "kernel/include", d) for d in ("api", "abi", "kernel")]

CONTROL = [
    ("kernel/lib/usb_audio_parse.c", "            if (stored && (out->uac2 ||", "            if (playable && (out->uac2 ||"),
    ("kernel/lib/usb_audio_parse.c", "    if (!keep) out->ep = 0;\n", ""),
    ("kernel/lib/scsi.c", "    return block_ok(block) && last < UINT64_MAX / (block / 512);", "    return block_ok(block);"),
]
UAC, CAP10, CAP16 = 0, 1, 2


def alt(ifn, a, ep, sub, bits):
    """One UAC1 streaming alternate: interface, AS_GENERAL, Type I at 48 kHz, an iso OUT."""
    d = bytes([9, 4, ifn, a, 1, 1, 2, 0, 0])
    d += bytes([7, 0x24, 1, 1, 1, 1, 0])
    d += bytes([11, 0x24, 2, 1, 2, sub, bits, 1]) + (48000).to_bytes(3, "little")
    d += bytes([9, 5, ep, 0x09, 0xC0, 0x00, 1, 0, 0])
    return d


def cases():
    control = bytes([9, 4, 0, 0, 0, 1, 1, 0, 0])
    yield "uac_default_unstored", UAC, control + b"".join(alt(1, i + 1, 0x01, 2, 16) for i in range(4)) + alt(2, 1, 0x02, 3, 24)
    yield "uac_one_format", UAC, control + alt(1, 1, 0x01, 2, 16)
    yield "cap16_last_all_ones", CAP16, (2**64 - 1).to_bytes(8, "big") + (512).to_bytes(4, "big")
    yield "cap16_4k_wraps", CAP16, (2**62).to_bytes(8, "big") + (4096).to_bytes(4, "big")
    yield "cap10_plain", CAP10, (0x0003FFFF).to_bytes(4, "big") + (512).to_bytes(4, "big")


def build(tmp, control):
    srcs = []
    for rel in ("kernel/lib/usb_audio_parse.c", "kernel/lib/scsi.c", "kernel/lib/string.c"):
        edits = [(a, b) for f, a, b in CONTROL if f == rel]
        srcs.append(hostcheck.stage(tmp, rel, edits=edits, apply=control, tool=TOOL))
    flags = SAN + tuple(x for i in INC for x in ("-iquote", i))
    return hostcheck.compile(tmp, "descfuzz", [os.path.join(HERE, "descfuzz.c"), os.path.join(HERE, "ktest_stub.c"), *srcs],
                             flags=flags, tool=TOOL)


def run(exe, tmp, name, kind, body, accept=False):
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(bytes([kind]) + body)
    r = subprocess.run([exe, path], capture_output=True, text=True, timeout=30)
    if r.returncode:
        lines = [ln for ln in r.stderr.splitlines() if "INVARIANT" in ln or "ERROR:" in ln or "runtime error" in ln]
        return lines[0] if lines else "exit %d" % r.returncode
    if accept and "uac: accepted, 1 format" not in r.stdout:
        return "refused a plain one-format stream"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    ap.add_argument("--iters", type=int, default=600)
    args = ap.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="usbdesc-") as tmp:
        exe = build(tmp, args.positive_control)
        corpus = list(cases())
        for name, kind, body in corpus:
            why = run(exe, tmp, name, kind, body, accept=name == "uac_one_format")
            print("  %-22s %s" % (name, "RED  " + why if why else "green"))
            if why:
                failures.append(name)
        rng = random.Random(20261112)
        bad = 0
        for i in range(args.iters):
            name, kind, body = corpus[i % len(corpus)]
            b = bytearray(body)
            for _ in range(rng.randint(1, 8)):
                if b and rng.random() < 0.8:
                    b[rng.randrange(len(b))] = rng.choice((0, 1, 2, 4, 9, 0x7F, 0x80, 0xFF, rng.randrange(256)))
                elif b:
                    del b[rng.randrange(len(b)):]
            why = run(exe, tmp, "mut", kind, bytes(b))
            if why:
                bad += 1
                if bad <= 3:
                    print("  mutation of %s: RED  %s" % (name, why))
        print("  %d mutations: %d red" % (args.iters, bad))
        if bad:
            failures.append("mutations")
    if args.positive_control:
        print("\n%s: POSITIVE CONTROL -- %d check(s) went red%s" %
              (TOOL, len(failures), "" if failures else " -- NONE: the check cannot see the break"))
        return 0 if failures else 1
    print("\n%s: %s" % (TOOL, "PASS" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
