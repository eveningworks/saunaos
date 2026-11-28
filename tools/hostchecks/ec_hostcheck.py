#!/usr/bin/env python3
"""The embedded controller's byte protocol against a simulated EC, on the host.

WHAT IS UNDER TEST. kernel/acpi/ec_proto.c -- RD_EC, WR_EC and QR_EC over
a command/status port and a data port, each wait polled with a bound
(docs/aml-design.md, stage 4b) -- compiled with the host gcc beside a
simulated EC. QEMU has no embedded controller, so this and the ASUS are
all that run it. The simulated EC takes each byte only some status reads
after it was written, as a real one does, and records a PROTOCOL
VIOLATION -- a byte written while its input buffer is full, a data read
with nothing in the output buffer -- which a real EC would silently
answer wrongly. It checks:

  - every one of the 256 bytes written and read back, at random speeds,
    with no violation;
  - QR_EC answers the queued events in order, then 0, and SCI_EVT clears;
  - a stale byte left in the output buffer is read away, not taken as
    the next answer;
  - an EC slower than the 1 ms spin is still answered, through naps;
  - an EC that never answers fails the access within the bound, and the
    next access, once it answers again, works.

POSITIVE CONTROL (--positive-control): the wait before each data byte is
dropped, and so is the flush of a stale byte -- the violation and the
stale-byte checks must go red.

    python3 tools/hostchecks/ec_hostcheck.py [--positive-control]
"""

# The EC byte protocol against a simulated EC, under ASan + UBSan. ~2 s.
RUNNER = {"covers": "the embedded controller's RD_EC/WR_EC/QR_EC protocol against a simulated EC", "gate": True}

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

TOOL = "ec_hostcheck"
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
       "-fno-sanitize-recover=undefined")
CONTROL = [
    ("    if (wait_for(e, EC_STS_IBF, 0)) return -1;\n    e->out(e->data, v);", "    e->out(e->data, v);"),
    ("    for (int i = 0; i < 4 && (e->in(e->cmd) & EC_STS_OBF); i++) {",
     "    for (int i = 0; i < 0 && (e->in(e->cmd) & EC_STS_OBF); i++) {"),
]

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "ec_proto.h"

// THE SIMULATED EC. A written byte sits in the input buffer (IBF) until
// `lat` status reads have passed; then the EC takes it. A command's
// answer appears in the output buffer (OBF) the same way.
enum { IDLE, RD_ADDR, WR_ADDR, WR_VAL };
static uint8_t ram[256], in_byte, out_byte, events[16];
static int ibf, obf, in_is_cmd, state, wr_addr, nev, lat, wait, hung, violations;
static uint64_t clock_ns, slow_until;
static unsigned rng = 20261128;

static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static uint64_t now(void) { return clock_ns; }

static void take(void) {   // the EC consumes the input byte
    ibf = 0;
    if (in_is_cmd) {
        switch (in_byte) {
        case EC_CMD_READ:  state = RD_ADDR; break;
        case EC_CMD_WRITE: state = WR_ADDR; break;
        case EC_CMD_QUERY:
            out_byte = nev ? events[0] : 0;
            if (nev) { for (int i = 1; i < nev; i++) events[i - 1] = events[i]; nev--; }
            obf = 1; state = IDLE; break;
        default: violations++; break;
        }
        return;
    }
    switch (state) {
    case RD_ADDR: out_byte = ram[in_byte]; obf = 1; state = IDLE; break;
    case WR_ADDR: wr_addr = in_byte; state = WR_VAL; break;
    case WR_VAL:  ram[wr_addr] = in_byte; state = IDLE; break;
    default: violations++; break;   // a data byte nobody asked for
    }
}

static uint8_t sim_in(uint16_t port) {
    clock_ns += 100;
    if (port == 0x66) {
        if (ibf && !hung && clock_ns >= slow_until && ++wait >= lat) take();
        return (uint8_t)((obf ? EC_STS_OBF : 0) | (ibf ? EC_STS_IBF : 0) | (nev ? EC_STS_SCI_EVT : 0));
    }
    if (!obf) { violations++; return 0xFF; }   // read with nothing there
    obf = 0;
    return out_byte;
}

static void sim_out(uint16_t port, uint8_t v) {
    clock_ns += 100;
    if (ibf) violations++;                     // written over a byte not yet taken
    in_byte = v; in_is_cmd = port == 0x66; ibf = 1; wait = 0;
    lat = (int)(rnd() % 6);
}

static void nap(void) { clock_ns += 1000000; }   // the kernel parks 1 ms

static struct ec_io io = { .cmd = 0x66, .data = 0x62, .in = sim_in, .out = sim_out, .now_ns = now,
                           .busy_ns = 1000000, .wait_ns = 500000000, .nap = nap };
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("RED  " __VA_ARGS__); printf("\n"); } \
                           else { printf("green " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    int bad = 0;
    for (int a = 0; a < 256; a++) bad |= ec_write(&io, (uint8_t)a, (uint8_t)(a * 7 + 3));
    for (int a = 0; a < 256; a++) {
        uint8_t v = 0;
        bad |= ec_read(&io, (uint8_t)a, &v);
        if (v != (uint8_t)(a * 7 + 3)) bad = 1;
    }
    CHECK(!bad && !violations, "every byte written and read back (violations %d)", violations);

    events[0] = 0x0A; events[1] = 0x6D; events[2] = 0x13; nev = 3;
    uint8_t q[4] = { 0 };
    for (int i = 0; i < 4; i++) bad |= ec_query(&io, &q[i]);
    CHECK(!bad && q[0] == 0x0A && q[1] == 0x6D && q[2] == 0x13 && q[3] == 0 && !violations,
          "QR_EC answers 0a 6d 13 then 0 (got %02x %02x %02x %02x, violations %d)", q[0], q[1], q[2], q[3], violations);
    CHECK(!(sim_in(0x66) & EC_STS_SCI_EVT), "SCI_EVT clears once every event is taken");

    out_byte = 0x5A; obf = 1;   // left by an abandoned transaction
    uint8_t v = 0;
    int rc = ec_read(&io, 0x10, &v);
    CHECK(rc == 0 && v == ram[0x10], "a stale output byte is read away first (got %02x, want %02x)", v, ram[0x10]);

    slow_until = clock_ns + 3000000;   // 3 ms: past the spin, so it naps
    rc = ec_read(&io, 0x30, &v);
    CHECK(rc == 0 && v == ram[0x30] && io.naps >= 2 && !io.timeouts,
          "an EC slower than the spin is answered through naps (%d naps)", (int)io.naps);

    hung = 1;
    uint64_t t0 = clock_ns;
    rc = ec_read(&io, 0x20, &v);
    CHECK(rc == -1 && io.timeouts >= 1 && clock_ns - t0 < 600000000,
          "an EC that never answers fails within the bound (%llu ns)", (unsigned long long)(clock_ns - t0));
    hung = 0;
    ibf = 0; state = IDLE;      // the EC recovers and drops what it held
    violations = 0;
    rc = ec_read(&io, 0x20, &v);
    CHECK(rc == 0 && v == ram[0x20] && !violations, "...and the next access works once it answers");
    printf("%d transactions, %d failed checks\n", (int)io.transactions, fails);
    return fails ? 1 : 0;
}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="ec-") as tmp:
        drv = hostcheck.write(tmp, "ec_sim.c", DRIVER)
        hostcheck.stage(tmp, "kernel/acpi/ec_proto.h", tool=TOOL)
        src = hostcheck.stage(tmp, "kernel/acpi/ec_proto.c", edits=CONTROL,
                              apply=args.positive_control, tool=TOOL)
        exe = hostcheck.compile(tmp, "ec_sim", [drv, src], flags=SAN, includes=[tmp], tool=TOOL)
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    for ln in r.stdout.splitlines():
        print("  " + ln)
    if r.stderr.strip():
        print(r.stderr.strip()[:2000])
    red = r.returncode != 0
    if args.positive_control:
        print("\n%s: POSITIVE CONTROL -- %s" % (TOOL, "went red" if red else "NOTHING went red: the check cannot see the break"))
        return 0 if red else 1
    print("\n%s: %s" % (TOOL, "FAILED" if red else "PASS"))
    return 1 if red else 0


if __name__ == "__main__":
    sys.exit(main())
