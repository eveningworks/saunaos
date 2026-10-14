#!/usr/bin/env python3
"""Check userland/ui/uui_caret.c -- the one blink every text caret shares --
with the host gcc, by driving it the way the compositor's loop does.

The caret asks for a repaint at each half-period, and the loop answers.
But the loop also paints for OTHER damage -- the taskbar clock's, once a
second -- and that frame runs every overlay's draw with the clip
elsewhere: `uui_caret_visible()` is called and the field's pixels do not
change. A caret that took that call for its own repaint stayed ON
through every "off" phase, locked to the clock, for the whole ten-second
blink (the Start menu's `the caret blinks`, intermittent since
2026-10-03).

So this simulates the loop -- park until the caret's deadline or the
next clock tick, paint the clock's clipped frame first, then ask the
caret -- with the clock at every 5 ms offset across its second, and
asserts what the SCREEN shows:

  - the field alternates, on in even half-periods and off in odd ones,
    for all twenty phases of the blink, at every clock alignment
  - it ends ON and solid, and asks for nothing more
  - a field that loses focus mid-blink stops the asking within one
    phase, and nothing spins (a repaint asked for and never drawn)

    python3 tools/caret_hostcheck.py
    python3 tools/caret_hostcheck.py --positive-control   # must FAIL

Needs only gcc and the standard library.
"""

import argparse
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

HALF_MS, PHASES, END_MS = 500, 20, 12000

STUB_SYS = "unsigned long long sys_monotonic_ns(void);\n"
STUB_SETTING = ("#include <stddef.h>\n"
                "int usetting_get(const char *name, char *out, size_t cap);\n")

# argv: clock_offset_ms lose_focus_at_ms (-1 = never) late_ms -- how long
# after its deadline the loop actually wakes. Prints "t on" at
# every change of what the field SHOWS, then "asks N".
DRIVER = r"""
#include "ui/uui_caret.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long g_now;
unsigned long long sys_monotonic_ns(void) { return g_now; }
int usetting_get(const char *name, char *out, size_t cap) {
    (void)name;
    strncpy(out, "on", cap);
    return 1;
}
// +1 ns: a t0 of exactly 0 means "no input yet" to the code under test.
static void at(long ms) { g_now = (unsigned long long)ms * 1000000ull + 1; }

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    long clock = atol(argv[1]), lose = atol(argv[2]), late = atol(argv[3]);
    const long end = """ + str(END_MS) + r""";
    long t = 0, tick = clock;
    int focused = 1, asks = 0, spins = 0;
    at(0);
    uui_caret_reset();
    int screen = uui_caret_visible();   // the input's own repaint
    printf("%ld %d\n", t, screen);
    for (;;) {
        if (lose >= 0 && t >= lose) focused = 0;
        int w = uui_caret_wait_ms();
        if (w == 0) {
            // The caret's own repaint: the field is drawn, and what it
            // shows is what visible() answers -- if it is still focused.
            asks++;
            if (++spins > 100) { printf("SPIN at %ld\n", t); return 1; }
            if (focused) {
                int v = uui_caret_visible();
                if (v != screen) { screen = v; printf("%ld %d\n", t, screen); }
            }
            continue;
        }
        spins = 0;
        long wake = w < 0 ? end + 1 : t + w;
        if (tick < wake) wake = tick;
        if (wake > end) break;
        t = wake + late;   // a loaded machine wakes the loop late
        at(t);
        if (tick <= t) {
            // The clock's frame: every overlay draws, clipped to the
            // clock -- visible() is asked, the field's pixels stay.
            tick += 1000;
            if (focused) (void)uui_caret_visible();
        }
    }
    printf("asks %d\n", asks);
    return 0;
}
"""


def build(tmp, poison=False):
    hostcheck.stage(tmp, "userland/ui/uui_caret.h", dest="ui/uui_caret.h")
    hostcheck.write(tmp, "rt/sys.h", STUB_SYS)
    hostcheck.write(tmp, "lib/usetting.h", STUB_SETTING)
    # The control: the logic this replaced, which counted ANY visible()
    # call in a phase as that phase's repaint.
    src = hostcheck.stage(tmp, "userland/ui/uui_caret.c", apply=poison,
                          tool="caret_hostcheck",
                          edits=[("if (p != g_asked) {", "if (p != g_drawn) {"),
                                 ("if (g_drawn < g_asked) return -1;",
                                  "if (p == g_asked) return -1;")])
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "caret_host", [src, drv], includes=[tmp],
                             tool="caret_hostcheck")


def run(exe, clock, lose=-1, late=0):
    r = subprocess.run([exe, str(clock), str(lose), str(late)],
                       capture_output=True, text=True)
    changes, asks, spin = [], None, None
    for line in r.stdout.split("\n"):
        if line.startswith("SPIN"):
            spin = line
        elif line.startswith("asks"):
            asks = int(line.split()[1])
        elif line.strip():
            t, v = line.split()
            changes.append((int(t), int(v)))
    return changes, asks, spin


def shown(changes, t):
    v = None
    for ct, cv in changes:
        if ct <= t:
            v = cv
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="compile the old repaint logic; the run must then fail")
    args = ap.parse_args()

    fails, checks = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, poison=args.positive_control)

        # On time, and 30 ms late -- a loaded guest's wake, which is what
        # widens the alignment that froze it from an instant to a window.
        for late in (0, 30):
            frozen = []
            for clock in range(0, 1000, 5):
                checks += 1
                changes, asks, spin = run(exe, clock, late=late)
                wrong = [k for k in range(PHASES)
                         if shown(changes, k * HALF_MS + HALF_MS // 2 + late) != (k % 2 == 0)]
                if spin:
                    fails.append(f"clock at +{clock} ms, {late} ms late: {spin}")
                elif wrong:
                    frozen.append(clock)
                elif shown(changes, END_MS) != 1:
                    fails.append(f"clock at +{clock} ms, {late} ms late: does not end ON")
                elif asks != PHASES:
                    fails.append(f"clock at +{clock} ms, {late} ms late: {asks} repaints "
                                 f"asked, want one per phase ({PHASES})")
            if frozen:
                fails.append(f"waking {late} ms late, the field does not blink with the "
                             f"clock's frame at {len(frozen)} of 200 offsets, e.g. +{frozen[0]} ms")

        # FOCUS LOST mid-blink (phase 3): one more ask at most, then quiet.
        checks += 1
        changes, asks, spin = run(exe, 250, lose=1700)
        if spin or asks is None or asks > 4:
            fails.append(f"a field that lost focus kept asking: asks={asks} {spin or ''}")

    if args.positive_control:
        if fails:
            print(f"caret_hostcheck: positive control FAILED as it must "
                  f"({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("caret_hostcheck: positive control passed -- the harness "
              "cannot see a caret held by the clock's frame", file=sys.stderr)
        return 1
    for f in fails:
        print("  FAIL  " + f)
    print(f"caret_hostcheck: {checks - len(fails)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
